#include "shell/osd/osd_overlay.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "shell/surface/edge_inset.h"
#include "shell/surface/shadow.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr Logger kLog("osd");

  constexpr int kHideDelayMs = Style::animSlow * 3 + Style::animFast * 2;

  // noctadark OSD panel (OSD.qml longHWidth/longHHeight) with the card inset by
  // Style.marginM * 1.5 on every side: 200 - 27 = 173 wide, 66 - 27 = 39 tall.
  // Base units; everything scales with the OSD ui scale.
  constexpr float kOsdCardWidthBase = 173.0f;
  constexpr float kOsdCardHeightBase = 39.0f;
  constexpr float kOsdContentInset = 9.0f;   // Style.marginM
  constexpr float kOsdContentMargin = 13.0f; // Style.marginL
  constexpr float kOsdFontSize = 12.0f;      // Style.fontSizeS
  constexpr float kOsdBorderWidth = 2.0f;    // Style.borderM, clamped to >= 2

  constexpr float kRevealScaleStart = 0.85f;
  constexpr float kRevealScaleEnd = 1.0f;

  [[nodiscard]] float osdUiScale(const ConfigService* config) {
    if (config == nullptr) {
      return 1.0f;
    }
    const auto& accessibility = config->config().accessibility;
    const auto& osd = config->config().osd;
    return std::max(0.1f, accessibility.uiScale * osd.scale);
  }

  [[nodiscard]] bool isOsdKindEnabled(const OsdKindsConfig& kinds, OsdKind kind) {
    switch (kind) {
    case OsdKind::Volume:
      return kinds.volume && kinds.volumeOutput;
    case OsdKind::Microphone:
      return kinds.volume && kinds.volumeInput;
    case OsdKind::Brightness:
      return kinds.brightness;
    case OsdKind::Wifi:
      return kinds.wifi;
    case OsdKind::Bluetooth:
      return kinds.bluetooth;
    case OsdKind::PowerProfile:
      return kinds.powerProfile;
    case OsdKind::Caffeine:
      return kinds.caffeine;
    case OsdKind::NightLight:
      return kinds.nightlight;
    case OsdKind::Dnd:
      return kinds.dnd;
    case OsdKind::LockKeys:
      return kinds.lockKeys;
    case OsdKind::KeyboardLayout:
      return kinds.keyboardLayout;
    case OsdKind::Media:
      return kinds.media;
    case OsdKind::Privacy:
      return kinds.privacy;
    case OsdKind::KeyboardBacklight:
      return kinds.keyboardBacklight;
    }
    return true;
  }

  [[nodiscard]] float osdBackgroundOpacity(const ConfigService* config) {
    if (config == nullptr) {
      return 0.97f;
    }
    return std::clamp(config->config().osd.backgroundOpacity, 0.0f, 1.0f);
  }

  [[nodiscard]] std::string effectiveOsdPosition(const ConfigService* config) {
    const std::string& position =
        (config != nullptr && !config->config().osd.position.empty()) ? config->config().osd.position : "top_center";
    return position;
  }

  [[nodiscard]] float osdCardWidth(float s) { return kOsdCardWidthBase * s; }

  [[nodiscard]] float osdCardHeight(float s) { return kOsdCardHeightBase * s; }

  [[nodiscard]] float osdBorderWidth(float s) {
    return static_cast<float>(std::max(2, static_cast<int>(std::lround(kOsdBorderWidth * s))));
  }

  [[nodiscard]] std::string osdFontFamily(const ConfigService* config) {
    if (config == nullptr) {
      return {};
    }
    const auto& osd = config->config().osd;
    if (osd.fontFamily.has_value() && !osd.fontFamily->empty()) {
      return *osd.fontFamily;
    }
    return config->config().shell.fontFamily;
  }

  [[nodiscard]] ShellConfig::ShadowConfig osdShadow(const ConfigService* config) {
    if (config == nullptr) {
      return {};
    }
    return config->config().shell.shadow;
  }

} // namespace

OsdOverlay::OsdOverlay() = default;

OsdOverlay::~OsdOverlay() = default;

void OsdOverlay::initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
  m_lastConfiguredEnabled = m_config == nullptr || m_config->config().osd.enabled;
}

void OsdOverlay::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "osd-enable",
      [this](const std::string& args) -> std::string {
        if (!noctalia::ipc::splitWords(args).empty()) {
          return "error: osd-enable takes no arguments\n";
        }
        setEnabledOverride(true);
        return "ok\n";
      },
      "", "Enable OSD popups"
  );
  ipc.registerHandler(
      "osd-disable",
      [this](const std::string& args) -> std::string {
        if (!noctalia::ipc::splitWords(args).empty()) {
          return "error: osd-disable takes no arguments\n";
        }
        setEnabledOverride(false);
        return "ok\n";
      },
      "", "Disable OSD popups"
  );
  ipc.registerHandler(
      "osd-toggle",
      [this](const std::string& args) -> std::string {
        if (!noctalia::ipc::splitWords(args).empty()) {
          return "error: osd-toggle takes no arguments\n";
        }
        setEnabledOverride(!isEnabled());
        return isEnabled() ? "on\n" : "off\n";
      },
      "", "Toggle OSD popups"
  );
}

bool OsdOverlay::isEnabled() const noexcept {
  const bool configuredEnabled = m_config == nullptr || m_config->config().osd.enabled;
  return m_runtimeEnabledOverride.value_or(configuredEnabled);
}

void OsdOverlay::setEnabledOverride(bool enabled) {
  m_runtimeEnabledOverride = enabled;
  if (!enabled) {
    destroySurfaces();
  }
}

void OsdOverlay::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void OsdOverlay::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestLayout();
    }
  }
}

void OsdOverlay::show(const OsdContent& content) {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }
  if (!isEnabled()) {
    return;
  }
  if (m_config != nullptr && !isOsdKindEnabled(m_config->config().osd.kinds, content.kind)) {
    return;
  }

  m_content = content;
  ensureSurfaces();
  for (auto& inst : m_instances) {
    if (inst->surface == nullptr) {
      continue;
    }
    inst->showPending = true;
    inst->surface->requestUpdate();
  }
}

bool OsdOverlay::isVisible() const {
  return std::ranges::any_of(m_instances, [](const auto& inst) {
    return inst->visible || inst->showPending || inst->showAnimId != 0;
  });
}

OsdOverlay::SurfaceMargins OsdOverlay::surfaceMarginsForPosition(const std::string& position) const {
  const int marginH = (m_config != nullptr) ? std::max(0, m_config->config().osd.offsetX) : 0;
  const int marginV = (m_config != nullptr) ? std::max(0, m_config->config().osd.offsetY) : 0;
  const float layoutScale = osdUiScale(m_config);
  const std::int32_t sideMargin = shell::surface_edge_inset::resolve(marginH, Style::spaceMd * layoutScale).layerMargin;

  SurfaceMargins margins{
      .top = marginV,
      .right = sideMargin,
      .bottom = 0,
      .left = 0,
  };

  if (position == "top_left") {
    margins.right = 0;
    margins.left = sideMargin;
  } else if (position == "top_center") {
    margins.right = 0;
  } else if (position == "bottom_left") {
    margins.top = 0;
    margins.right = 0;
    margins.bottom = marginV;
    margins.left = sideMargin;
  } else if (position == "bottom_center") {
    margins.top = 0;
    margins.right = 0;
    margins.bottom = marginV;
  } else if (position == "bottom_right") {
    margins.top = 0;
    margins.bottom = marginV;
  } else if (position == "center_left") {
    margins.top = 0;
    margins.right = 0;
    margins.left = sideMargin;
  } else if (position == "center_right") {
    margins.top = 0;
    margins.bottom = 0;
  }

  return margins;
}

std::vector<std::string> OsdOverlay::osdMonitors() const {
  if (m_config == nullptr) {
    return {};
  }
  return m_config->config().osd.monitors;
}

bool OsdOverlay::shouldRenderOnOutput(const WaylandOutput& output) const {
  const auto selectedMonitors = osdMonitors();
  if (selectedMonitors.empty()) {
    return true;
  }
  return std::ranges::any_of(selectedMonitors, [&output](const std::string& match) {
    return outputMatchesSelector(match, output);
  });
}

void OsdOverlay::onOutputChange() {
  if (m_instances.empty()) {
    return;
  }
  ensureSurfaces();
  requestLayout();
}

void OsdOverlay::onConfigReload() {
  const bool configuredEnabled = m_config == nullptr || m_config->config().osd.enabled;
  if (configuredEnabled != m_lastConfiguredEnabled) {
    m_runtimeEnabledOverride.reset();
    m_lastConfiguredEnabled = configuredEnabled;
  }
  if (!isEnabled()) {
    destroySurfaces();
    return;
  }
  onOutputChange();
}

void OsdOverlay::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }

  const std::string position = effectiveOsdPosition(m_config);
  const float layoutScale = osdUiScale(m_config);
  const auto selectedMonitors = osdMonitors();

  if (!m_instances.empty() && (position != m_lastPosition || selectedMonitors != m_lastMonitorSelectors)) {
    destroySurfaces();
  }

  if (!m_instances.empty() && std::abs(layoutScale - m_lastLayoutScale) > 1.0e-4f) {
    destroySurfaces();
  }

  const float cw = osdCardWidth(layoutScale);
  const float ch = osdCardHeight(layoutScale);
  const auto geometry = popup_chrome::computeGeometry(cw, ch, osdShadow(m_config), true);
  const auto surfaceWidth = geometry.surfaceWidth;
  const auto surfaceHeight = geometry.surfaceHeight;
  const SurfaceMargins margins = surfaceMarginsForPosition(position);

  m_lastPosition = position;
  m_lastLayoutScale = layoutScale;
  m_lastMonitorSelectors = selectedMonitors;

  const bool anyConfiguredPresent =
      selectedMonitors.empty()
      || std::any_of(m_wayland->outputs().begin(), m_wayland->outputs().end(), [this](const WaylandOutput& output) {
           return output.done && output.output != nullptr && output.hasUsableGeometry() && shouldRenderOnOutput(output);
         });

  std::erase_if(m_instances, [this, anyConfiguredPresent](const std::unique_ptr<Instance>& inst) {
    if (inst->output == nullptr) {
      return true;
    }
    const WaylandOutput* wlOutput = m_wayland->findOutputByWl(inst->output);
    if (wlOutput == nullptr) {
      return true;
    }
    if (!wlOutput->done || !wlOutput->hasUsableGeometry()) {
      return true;
    }
    return anyConfiguredPresent && !shouldRenderOnOutput(*wlOutput);
  });

  for (auto& inst : m_instances) {
    if (inst->surface == nullptr) {
      continue;
    }
    if (inst->surface->marginTop() != margins.top
        || inst->surface->marginRight() != margins.right
        || inst->surface->marginBottom() != margins.bottom
        || inst->surface->marginLeft() != margins.left) {
      inst->surface->setMargins(margins.top, margins.right, margins.bottom, margins.left);
    }
    if (inst->surface->width() != surfaceWidth || inst->surface->height() != surfaceHeight) {
      inst->surface->requestSize(surfaceWidth, surfaceHeight);
    }
  }

  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }
    if (anyConfiguredPresent && !shouldRenderOnOutput(output)) {
      continue;
    }

    auto existingIt = std::ranges::find_if(m_instances, [&output](const auto& inst) {
      return inst != nullptr && inst->output == output.output;
    });
    if (existingIt != m_instances.end()) {
      (*existingIt)->scale = output.scale;
      (*existingIt)->uiLayoutScale = layoutScale;
      continue;
    }

    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->scale = output.scale;
    inst->uiLayoutScale = layoutScale;

    std::uint32_t anchor = LayerShellAnchor::Top | LayerShellAnchor::Right;

    if (position == "top_left") {
      anchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
    } else if (position == "top_center") {
      anchor = LayerShellAnchor::Top;
    } else if (position == "bottom_left") {
      anchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
    } else if (position == "bottom_center") {
      anchor = LayerShellAnchor::Bottom;
    } else if (position == "bottom_right") {
      anchor = LayerShellAnchor::Bottom | LayerShellAnchor::Right;
    } else if (position == "center_left") {
      anchor = LayerShellAnchor::Left;
    } else if (position == "center_right") {
      anchor = LayerShellAnchor::Right;
    }

    auto surfaceConfig = LayerSurfaceConfig{
        .nameSpace = "mono-shell-osd",
        .layer = LayerShellLayer::Overlay,
        .anchor = anchor,
        .width = surfaceWidth,
        .height = surfaceHeight,
        .exclusiveZone = 0,
        .marginTop = margins.top,
        .marginRight = margins.right,
        .marginBottom = margins.bottom,
        .marginLeft = margins.left,
        .keyboard = LayerShellKeyboard::None,
        .defaultWidth = surfaceWidth,
        .defaultHeight = surfaceHeight,
        .prewarmBlur = true,
    };

    inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
    inst->surface->setRenderContext(m_renderContext);
    auto* instPtr = inst.get();
    inst->surface->setConfigureCallback([instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
      instPtr->surface->requestLayout();
    });
    inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
      prepareFrame(*instPtr, needsUpdate, needsLayout);
    });
    inst->surface->setFrameTickCallback([this, instPtr](float /*deltaMs*/) {
      if (instPtr->animations.hasActive()) {
        updateBlurRegion(*instPtr);
      }
    });
    inst->surface->setAnimationManager(&inst->animations);

    if (!inst->surface->initialize(output.output)) {
      kLog.warn("osd overlay: failed to initialize surface on {}", output.connectorName);
      continue;
    }

    inst->surface->setInputRegion({});
    inst->wlSurface = inst->surface->wlSurface();
    m_instances.push_back(std::move(inst));
  }
}

void OsdOverlay::destroySurfaces() {
  for (auto& inst : m_instances) {
    inst->animations.cancelAll();
  }
  m_instances.clear();
}

void OsdOverlay::prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || inst.surface == nullptr) {
    return;
  }

  const auto width = inst.surface->width();
  const auto height = inst.surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(inst.surface->renderTarget());

  const bool needsSceneBuild = inst.sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(inst.sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(inst.sceneRoot->height())) != height;
  if (needsSceneBuild) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    buildScene(inst, width, height);
  }

  if ((needsUpdate || needsLayout || needsSceneBuild) && inst.sceneRoot != nullptr) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    updateInstanceContent(inst);
  }

  if (needsUpdate && inst.showPending) {
    animateInstance(inst);
    inst.showPending = false;
  }

  // Keep blur publication after animation state/positions are applied for this frame.
  updateBlurRegion(inst);
}

void OsdOverlay::buildScene(Instance& inst, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("OsdOverlay::buildScene");
  if (m_renderContext == nullptr) {
    return;
  }

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const float s = inst.uiLayoutScale;
  const float cw = osdCardWidth(s);
  const float ch = osdCardHeight(s);
  const ShellConfig::ShadowConfig shadow = osdShadow(m_config);
  const auto geometry = popup_chrome::computeGeometry(cw, ch, shadow, true);

  inst.sceneRoot = ui::node({});
  inst.sceneRoot->setSize(w, h);
  inst.sceneRoot->setOpacity(1.0f);
  inst.surface->setSceneRoot(inst.sceneRoot.get());

  const float backgroundOpacity = osdBackgroundOpacity(m_config);
  (void)popup_chrome::addShadow(*inst.sceneRoot, geometry, shadow, 0.0f, backgroundOpacity);

  auto cardGroup = ui::node({.out = &inst.cardGroup});
  cardGroup->setPosition(geometry.contentX(), geometry.contentY());
  cardGroup->setFrameSize(cw, ch);
  cardGroup->setTransformOrigin(cw * 0.5f, ch * 0.5f);
  cardGroup->setOpacity(0.0f);
  cardGroup->setScale(kRevealScaleStart);
  inst.sceneRoot->addChild(std::move(cardGroup));

  const float border = osdBorderWidth(s);
  inst.cardGroup->addChild(
      ui::box({
          .out = &inst.background,
          .width = cw,
          .height = ch,
          .configure = [backgroundOpacity, border](Box& box) {
            box.setFill(colorSpecFromRole(ColorRole::Surface, backgroundOpacity));
            box.setBorder(colorSpecFromRole(ColorRole::Outline), border);
            box.setRadius(0.0f);
            box.setZIndex(0);
          },
      })
  );

  const std::string fontFamily = osdFontFamily(m_config);
  auto row = ui::row({
      .out = &inst.row,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = kOsdContentInset * s,
      .paddingV = kOsdContentInset * s,
      .paddingH = (kOsdContentInset + kOsdContentMargin) * s,
      .width = cw,
      .height = ch,
      .configure = [](Flex& flex) { flex.setZIndex(1); },
  });

  auto label = ui::label({
      .out = &inst.label,
      .text = "Volume",
      .fontSize = kOsdFontSize * s,
      .fontFamily = fontFamily,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
      .textAlign = TextAlign::Start,
      .flexGrow = 1.0f,
      .configure = [](Label& l) { l.setZIndex(1); },
  });

  auto value = ui::label({
      .out = &inst.value,
      .text = "100%",
      .fontSize = kOsdFontSize * s,
      .fontFamily = fontFamily,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
      .textAlign = TextAlign::End,
      .configure = [](Label& l) { l.setZIndex(1); },
  });

  row->addChild(std::move(label));
  row->addChild(std::move(value));
  inst.cardGroup->addChild(std::move(row));
}

void OsdOverlay::updateInstanceContent(Instance& inst) {
  if (m_renderContext == nullptr
      || inst.background == nullptr
      || inst.row == nullptr
      || inst.label == nullptr
      || inst.value == nullptr) {
    return;
  }

  const float s = inst.uiLayoutScale;
  const float cw = osdCardWidth(s);

  inst.background->setFill(colorSpecFromRole(ColorRole::Surface, osdBackgroundOpacity(m_config)));
  inst.background->setBorder(colorSpecFromRole(ColorRole::Outline), osdBorderWidth(s));

  const ColorRole labelRole = ColorRole::OnSurface;
  const ColorRole valueRole = ColorRole::OnSurface;

  const std::string fontFamily = osdFontFamily(m_config);
  inst.label->setFontFamily(fontFamily);
  inst.value->setFontFamily(fontFamily);

  const bool hasLabel = !m_content.label.empty();
  inst.label->setVisible(hasLabel);
  inst.label->setText(m_content.label);
  inst.label->setColor(colorSpecFromRole(labelRole));
  inst.label->setFlexGrow(hasLabel ? 1.0f : 0.0f);
  inst.label->setTextAlign(TextAlign::Start);

  inst.value->setText(m_content.value);
  inst.value->setColor(colorSpecFromRole(valueRole));
  inst.value->setTextAlign(hasLabel ? TextAlign::End : TextAlign::Center);
  inst.row->setJustify(hasLabel ? FlexJustify::Start : FlexJustify::Center);

  // Reserve width for "100%" so the card doesn't shift as the percentage changes.
  inst.value->setMinWidth(0.0f);
  if (hasLabel && m_content.showProgress) {
    inst.value->setText("100%");
    inst.value->measure(*m_renderContext);
    inst.value->setMinWidth(inst.value->width());
    inst.value->setText(m_content.value);
  }

  const float usableWidth = cw - (kOsdContentInset + kOsdContentMargin) * 2.0f * s;
  inst.value->setMaxWidth(std::max(0.0f, usableWidth));

  inst.row->layout(*m_renderContext);
}

void OsdOverlay::updateBlurRegion(Instance& inst) const {
  if (inst.surface == nullptr || inst.background == nullptr || inst.sceneRoot == nullptr) {
    return;
  }
  if (!inst.visible && !inst.showPending && inst.showAnimId == 0 && inst.hideAnimId == 0) {
    inst.surface->clearBlurRegion();
    return;
  }

  float ax = 0.0f;
  float ay = 0.0f;
  Node::absolutePosition(inst.background, ax, ay);
  const int rx = static_cast<int>(std::floor(ax));
  const int ry = static_cast<int>(std::floor(ay));
  const int rw = std::max(1, static_cast<int>(std::ceil(inst.background->width())));
  const int rh = std::max(1, static_cast<int>(std::ceil(inst.background->height())));
  inst.surface->setBlurRegion(Surface::tessellateRoundedRect(rx, ry, rw, rh, 0));
}

void OsdOverlay::applyReveal(Instance& inst, float reveal) {
  if (inst.cardGroup == nullptr) {
    return;
  }
  const float r = std::clamp(reveal, 0.0f, 1.0f);
  inst.cardGroup->setOpacity(r);
  inst.cardGroup->setScale(kRevealScaleStart + (kRevealScaleEnd - kRevealScaleStart) * r);
}

void OsdOverlay::animateInstance(Instance& inst) {
  if (inst.sceneRoot == nullptr) {
    return;
  }

  if (inst.hideAnimId != 0) {
    inst.animations.cancel(inst.hideAnimId);
    inst.hideAnimId = 0;
  }

  if (!inst.visible) {
    // During fast updates (e.g. slider drag), don't restart the show animation
    // every tick; keep the current show motion and only extend hide timing.
    if (inst.showAnimId == 0) {
      applyReveal(inst, 0.0f);
      inst.showAnimId = inst.animations.animate(
          0.0f, 1.0f, Style::animNormal, Easing::EaseInOutQuad, [this, &inst](float v) { applyReveal(inst, v); },
          [&inst]() {
            inst.showAnimId = 0;
            inst.visible = true;
          }
      );
    }
  } else {
    applyReveal(inst, 1.0f);
  }

  inst.hideAnimId = inst.animations.animateTimer(
      1.0f, 0.0f, kHideDelayMs, Easing::Linear, [](float /*v*/) {},
      [this, &inst]() {
        inst.hideAnimId = inst.animations.animate(
            1.0f, 0.0f, Style::animNormal, Easing::EaseInOutQuad, [this, &inst](float v) { applyReveal(inst, v); },
            [this, &inst]() {
              inst.hideAnimId = 0;
              inst.visible = false;
              DeferredCall::callLater([this]() {
                const bool allIdle = std::ranges::all_of(m_instances, [](const auto& i) {
                  return !i->visible && !i->showPending && i->showAnimId == 0 && i->hideAnimId == 0;
                });
                if (allIdle) {
                  destroySurfaces();
                }
              });
            }
        );
      }
  );
}
