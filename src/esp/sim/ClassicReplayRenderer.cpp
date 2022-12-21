// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ClassicReplayRenderer.h"

#include "esp/assets/ResourceManager.h"
#include "esp/gfx/RenderTarget.h"
#include "esp/gfx/Renderer.h"
#include "esp/metadata/MetadataMediator.h"
#include "esp/sensor/CameraSensor.h"
#include "esp/sensor/SensorFactory.h"
#include "esp/sim/SimulatorConfiguration.h"

#include <Magnum/GL/Context.h>
#include <Magnum/ImageView.h>

namespace esp {
namespace sim {

ClassicReplayRenderer::ClassicReplayRenderer(
    const ReplayRendererConfiguration& cfg) {
  config_ = cfg;
  SimulatorConfiguration simConfig;
  simConfig.createRenderer = true;
  auto metadataMediator = metadata::MetadataMediator::create(simConfig);
  assets::ResourceManager::Flags flags{};
  resourceManager_ =
      std::make_unique<assets::ResourceManager>(metadataMediator, flags);

  sceneManager_ = scene::SceneManager::create_unique();

  class SceneGraphPlayerImplementation
      : public gfx::replay::AbstractSceneGraphPlayerImplementation {
   public:
    SceneGraphPlayerImplementation(ClassicReplayRenderer& self, unsigned envIdx)
        : self_{self}, envIdx_{envIdx} {}

   private:
    gfx::replay::NodeHandle loadAndCreateRenderAssetInstance(
        const esp::assets::AssetInfo& assetInfo,
        const esp::assets::RenderAssetInstanceCreationInfo& creation) override {
      return self_.loadAndCreateRenderAssetInstance(envIdx_, assetInfo,
                                                    creation);
    }
    void changeLightSetup(const gfx::LightSetup& lights) override {
      return self_.resourceManager_->setLightSetup(lights);
    }

    ClassicReplayRenderer& self_;
    unsigned envIdx_;
  };

  for (int envIdx = 0; envIdx < config_.numEnvironments; ++envIdx) {
    auto sceneID = sceneManager_->initSceneGraph();
    auto semanticSceneID = cfg.forceSeparateSemanticSceneGraph
                               ? sceneManager_->initSceneGraph()
                               : sceneID;

    auto& sceneGraph = sceneManager_->getSceneGraph(sceneID);
    auto& parentNode = sceneGraph.getRootNode().createChild();
    auto sensorMap = esp::sensor::SensorFactory::createSensors(
        parentNode, cfg.sensorSpecifications);

    /* This used to use designated initializers. However, GCC 7.3 says
       "sorry, unimplemented: non-trivial designated initializers not
       supported" so I revert back to standard C++14. It also means I have to
       create a constructor so the Player gets a non-null PlayerImplementation
       reference. */
    EnvironmentRecord e{
        std::make_unique<SceneGraphPlayerImplementation>(*this, envIdx)};
    e.sceneID_ = sceneID, e.semanticSceneID_ = semanticSceneID;
    e.sensorParentNode_ = &parentNode;
    e.sensorMap_ = std::move(sensorMap);
    envs_.push_back(std::move(e));
  }

  // OpenGL context and renderer
  {
    // TODO make this more robust regarding the standalone option (assuming
    //  a context is there if standalone, assuming it's not if not)
    if (!Magnum::GL::Context::hasCurrent()) {
      context_ = gfx::WindowlessContext::create_unique(config_.gpuDeviceId);
    }

    gfx::Renderer::Flags flags;
#ifdef ESP_BUILD_WITH_BACKGROUND_RENDERER
    if (context_)
      flags |= gfx::Renderer::Flag::BackgroundRenderer;

    if (context_ && config_.leaveContextWithBackgroundRenderer)
      flags |= gfx::Renderer::Flag::LeaveContextWithBackgroundRenderer;
#else
    if (config_.numEnvironments > 1)
      ESP_DEBUG()
          << "ReplayBatchRenderer created without a background renderer. "
             "Multiple environments require a background renderer.";
#endif
    renderer_ = gfx::Renderer::create(context_.get(), flags);

    renderer_->acquireGlContext();
  }

  // Bind things to other things
  for (int envIndex = 0; envIndex < config_.numEnvironments; envIndex++) {
    auto& sensorMap = getEnvironmentSensors(envIndex);
    CORRADE_INTERNAL_ASSERT(sensorMap.size() == 1);
    for (auto& kv : sensorMap) {
      CORRADE_INTERNAL_ASSERT(kv.second.get().isVisualSensor());
      auto& visualSensor =
          static_cast<esp::sensor::VisualSensor&>(kv.second.get());
      renderer_->bindRenderTarget(visualSensor);
    }
  }
}

ClassicReplayRenderer::~ClassicReplayRenderer() {
  for (int envIdx = 0; envIdx < config_.numEnvironments; ++envIdx) {
    envs_[envIdx].player_.close();
    auto& sensorMap = envs_[envIdx].sensorMap_;
    for (auto& sensorPair : sensorMap) {
      sensor::SensorFactory::deleteSensor(sensorPair.second);
    }
  }
  resourceManager_.reset();
}

unsigned ClassicReplayRenderer::doEnvironmentCount() const {
  return envs_.size();
}

Mn::Vector2i ClassicReplayRenderer::doSensorSize(unsigned envIndex) {
  CORRADE_INTERNAL_ASSERT(envIndex >= 0 && envIndex < envs_.size());
  auto& env = envs_[envIndex];

  CORRADE_INTERNAL_ASSERT(env.sensorMap_.size() == 1);
  return static_cast<esp::sensor::VisualSensor&>(
             env.sensorMap_.begin()->second.get())
      .framebufferSize();
}

gfx::replay::Player& ClassicReplayRenderer::doPlayerFor(unsigned envIndex) {
  return envs_[envIndex].player_;
}

void ClassicReplayRenderer::doSetSensorTransform(unsigned envIndex,
                                                 const std::string& sensorName,
                                                 const Mn::Matrix4& transform) {
  auto& env = envs_[envIndex];

  ESP_CHECK(env.sensorMap_.count(sensorName),
            "ReplayRenderer::setSensorTransform: sensor " << sensorName
                                                          << " not found.");

  // note: can't use operator[] with map of reference_wrappers
  auto& thingy = env.sensorMap_.at(sensorName).get();
  auto& sensor = static_cast<sensor::VisualSensor&>(thingy);

  sensor.node().setTransformation(transform);
}

void ClassicReplayRenderer::doSetSensorTransformsFromKeyframe(
    unsigned envIndex,
    const std::string& prefix) {
  const auto& env = envs_[envIndex];
  for (const auto& kv : env.sensorMap_) {
    const auto sensorName = kv.first;
    sensor::VisualSensor& sensor =
        static_cast<sensor::VisualSensor&>(kv.second.get());

    std::string userName = prefix + sensorName;
    Mn::Vector3 translation;
    Mn::Quaternion rotation;
    bool found =
        env.player_.getUserTransform(userName, &translation, &rotation);
    ESP_CHECK(found,
              "setSensorTransformsFromKeyframe: couldn't find user transform \""
                  << userName << "\" for environment " << envIndex << ".");
    sensor.node().setRotation(rotation);
    sensor.node().setTranslation(translation);
  }
}

void ClassicReplayRenderer::doRender(
    Cr::Containers::ArrayView<const Mn::MutableImageView2D> imageViews) {
  for (int envIndex = 0; envIndex < config_.numEnvironments; envIndex++) {
    auto& sensorMap = getEnvironmentSensors(envIndex);
    CORRADE_INTERNAL_ASSERT(sensorMap.size() == 1);
    for (auto& kv : sensorMap) {
      CORRADE_INTERNAL_ASSERT(kv.second.get().isVisualSensor());
      auto& visualSensor =
          static_cast<esp::sensor::VisualSensor&>(kv.second.get());

      auto& sceneGraph = getSceneGraph(envIndex);

#ifdef ESP_BUILD_WITH_BACKGROUND_RENDERER
      // todo: investigate flags (frustum culling?)
      renderer_->enqueueAsyncDrawJob(visualSensor, sceneGraph,
                                     imageViews[envIndex],
                                     esp::gfx::RenderCamera::Flags{});
#else
      // TODO what am I supposed to do here?
      CORRADE_ASSERT_UNREACHABLE("Not implemented yet, sorry.", );
#endif
    }
  }

#ifdef ESP_BUILD_WITH_BACKGROUND_RENDERER
  renderer_->startDrawJobs();
  renderer_->waitDrawJobs();
#endif
}

void ClassicReplayRenderer::doRender(
    Magnum::GL::AbstractFramebuffer& framebuffer) {
  const Mn::Vector2i gridSize = environmentGridSize(config_.numEnvironments);

  for (int envIndex = 0; envIndex < config_.numEnvironments; envIndex++) {
    auto& sensorMap = getEnvironmentSensors(envIndex);
    CORRADE_INTERNAL_ASSERT(sensorMap.size() == 1);
    CORRADE_INTERNAL_ASSERT(sensorMap.begin()->second.get().isVisualSensor());
    auto& visualSensor = static_cast<esp::sensor::VisualSensor&>(
        sensorMap.begin()->second.get());

    visualSensor.renderTarget().renderEnter();

    auto& sceneGraph = getSceneGraph(envIndex);
    renderer_->draw(*visualSensor.getRenderCamera(), sceneGraph,
                    esp::gfx::RenderCamera::Flags{});

    visualSensor.renderTarget().renderExit();

    // TODO this is calculating the size from scratch for every environment in
    //  a hope that all have the same, figure out a better way
    const auto size =
        Mn::Vector2i{visualSensor.specification()->resolution}.flipped();
    const auto rectangle = Mn::Range2Di::fromSize(
        size * Mn::Vector2i{envIndex % gridSize.x(), envIndex / gridSize.x()},
        size);
    visualSensor.renderTarget().blitRgbaTo(framebuffer, rectangle);
  }
}

esp::scene::SceneNode* ClassicReplayRenderer::getEnvironmentSensorParentNode(
    unsigned envIndex) const {
  CORRADE_INTERNAL_ASSERT(envIndex < envs_.size());
  const auto& env = envs_[envIndex];
  return env.sensorParentNode_;
}

std::map<std::string, std::reference_wrapper<esp::sensor::Sensor>>&
ClassicReplayRenderer::getEnvironmentSensors(unsigned envIndex) {
  CORRADE_INTERNAL_ASSERT(envIndex < envs_.size());
  auto& env = envs_[envIndex];
  return env.sensorMap_;
}

esp::scene::SceneGraph& ClassicReplayRenderer::getSceneGraph(
    unsigned envIndex) {
  CORRADE_INTERNAL_ASSERT(envIndex < envs_.size());
  const auto& env = envs_[envIndex];
  return sceneManager_->getSceneGraph(env.sceneID_);
}

esp::scene::SceneGraph& ClassicReplayRenderer::getSemanticSceneGraph(
    unsigned envIndex) {
  CORRADE_INTERNAL_ASSERT(envIndex < envs_.size());
  const auto& env = envs_[envIndex];
  return sceneManager_->getSceneGraph(env.semanticSceneID_ == ID_UNDEFINED
                                          ? env.sceneID_
                                          : env.semanticSceneID_);
}

gfx::replay::NodeHandle ClassicReplayRenderer::loadAndCreateRenderAssetInstance(
    unsigned envIndex,
    const assets::AssetInfo& assetInfo,
    const assets::RenderAssetInstanceCreationInfo& creation) {
  // Note this pattern of passing the scene manager and two scene ids to
  // resource manager. This is similar to ResourceManager::loadStage.
  CORRADE_INTERNAL_ASSERT(envIndex >= 0 && envIndex < envs_.size());
  const auto& env = envs_[envIndex];
  // perf todo: avoid dynamic mem alloc
  std::vector<int> tempIDs{env.sceneID_, env.semanticSceneID_};
  auto* node = resourceManager_->loadAndCreateRenderAssetInstance(
      assetInfo, creation, sceneManager_.get(), tempIDs);
  return reinterpret_cast<gfx::replay::NodeHandle>(node);
}

}  // namespace sim
}  // namespace esp