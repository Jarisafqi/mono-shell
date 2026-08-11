#include "shell/bar/widgets/battery_widget.h"

#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <sstream>
#include <string>
#include <utility>

namespace {

  constexpr float kGraphicBodyWidth = 22.0f;
  constexpr float kGraphicBodyHeight = 14.0f;
  constexpr float kGraphicTerminalWidth = 2.5f;
  constexpr float kGraphicTerminalHeight = 7.0f;
  constexpr float kGraphicCornerRadius = 3.0f;

  ColorSpec withOpacity(ColorSpec color, float opacity) {
    color.alpha *= opacity;
    return color;
  }

  const char* batteryStateGlyph(BatteryState state) {
    if (state == BatteryState::Charging) {
      return "bolt-filled";
    }
    if (state == BatteryState::FullyCharged || state == BatteryState::PendingCharge) {
      return "plug-filled";
    }
    return nullptr;
  }

  const char* bluetoothDeviceGlyph(BluetoothDeviceKind kind) {
    switch (kind) {
    case BluetoothDeviceKind::Headset:
      return "bluetooth-device-headset";
    case BluetoothDeviceKind::Headphones:
      return "bluetooth-device-headphones";
    case BluetoothDeviceKind::Earbuds:
      return "bluetooth-device-earbuds";
    case BluetoothDeviceKind::Speaker:
      return "bluetooth-device-speaker";
    case BluetoothDeviceKind::Microphone:
      return "bluetooth-device-microphone";
    case BluetoothDeviceKind::Mouse:
      return "bluetooth-device-mouse";
    case BluetoothDeviceKind::Keyboard:
      return "bluetooth-device-keyboard";
    case BluetoothDeviceKind::Phone:
      return "bluetooth-device-phone";
    case BluetoothDeviceKind::Computer:
      return "device-laptop";
    case BluetoothDeviceKind::Gamepad:
      return "bluetooth-device-gamepad";
    case BluetoothDeviceKind::Watch:
      return "bluetooth-device-watch";
    case BluetoothDeviceKind::Tv:
      return "bluetooth-device-tv";
    case BluetoothDeviceKind::Unknown:
    default:
      return "bluetooth-device-generic";
    }
  }

  std::string formatCompactDuration(std::int64_t seconds) {
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const std::string hourText = i18n::tr("time.units.hour-compact", "count", hours);
    const std::string minuteText = i18n::tr("time.units.minute-compact", "count", minutes);
    if (hours > 0 && minutes > 0) {
      return i18n::tr("time.duration.two-parts", "first", hourText, "second", minuteText);
    }
    if (hours > 0) {
      return hourText;
    }
    if (minutes > 0) {
      return minuteText;
    }
    return i18n::tr("time.duration.less-than-minute");
  }

} // namespace

BatteryWidget::BatteryWidget(UPowerService* upower, BluetoothService* bluetooth, Options options)
    : m_upower(upower), m_bluetooth(bluetooth), m_deviceSelector(std::move(options.deviceSelector)),
      m_warningThreshold(options.warningThreshold), m_warningColor(options.warningColor),
      m_displayMode(options.displayMode), m_labelContent(options.labelContent), m_showLabel(options.showLabel),
      m_hideWhenPlugged(options.hideWhenPlugged), m_hideWhenFull(options.hideWhenFull),
      m_showBluetoothDevices(options.showBluetoothDevices), m_bluetoothGap(options.bluetoothGap),
      m_hPadding(options.hPadding), m_tooltip(options.tooltip) {}

// Vertical bars are too narrow for time or rate text, so they always show the bare percentage; the
// tooltip carries the full detail. Time and rate are only known while the battery is actively charging
// or discharging, and the percentage stands in whenever the selected content has no value to show.
std::string BatteryWidget::buildLabelText(int pct, const UPowerState& state) const {
  if (m_isVertical) {
    return std::to_string(pct);
  }

  switch (m_labelContent) {
  case BatteryLabelContent::Time:
    if (state.state == BatteryState::Discharging && state.timeToEmpty > 0) {
      return formatCompactDuration(state.timeToEmpty);
    }
    if (state.state == BatteryState::Charging && state.timeToFull > 0) {
      return formatCompactDuration(state.timeToFull);
    }
    break;
  case BatteryLabelContent::Rate:
    if (state.energyRate > 0.0) {
      return std::format("{:.1f} W", state.energyRate);
    }
    break;
  case BatteryLabelContent::Percent:
    break;
  }
  return std::format("{}%", pct);
}

void BatteryWidget::create() {
  auto container = ui::inputArea({});
  setRoot(std::move(container));

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    createGraphicMode();
  } else if (m_displayMode == BatteryDisplayMode::Glyph) {
    createGlyphMode();
  } else {
    createLabelOnlyMode();
  }

  if (m_showBluetoothDevices && m_bluetooth != nullptr) {
    auto* rootNode = static_cast<InputArea*>(root());
    rootNode->addChild(
        ui::glyph({
            .out = &m_bluetoothGlyph,
            .glyph = "bluetooth-device-generic",
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
            .visible = false,
        })
    );
    rootNode->addChild(
        ui::label({
            .out = &m_bluetoothLabel,
            .fontSize = Style::fontSizeBody * m_contentScale,
            .fontWeight = labelFontWeight(),
            .fontFamily = labelFontFamily(),
            .visible = false,
        })
    );
  }
}

void BatteryWidget::createGraphicMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::box({
          .out = &m_bodyBg,
          .fill = withOpacity(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.3f),
      })
  );

  container->addChild(
      ui::box({
          .out = &m_fillRect,
      })
  );

  container->addChild(
      ui::box({
          .out = &m_terminalNub,
          .fill = withOpacity(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.3f),
      })
  );

  if (m_showLabel) {
    container->addChild(
        ui::label({
            .out = &m_overlayLabel,
            .fontWeight = labelFontWeight(),
            .fontFamily = labelFontFamily(),
            .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  container->addChild(
      ui::glyph({
          .out = &m_overlayGlyph,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .visible = false,
      })
  );
}

void BatteryWidget::createGlyphMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "battery-4",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  container->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = m_showLabel,
      })
  );
}

void BatteryWidget::createLabelOnlyMode() {
  auto* container = static_cast<InputArea*>(root());

  container->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = m_showLabel,
      })
  );
}

void BatteryWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (rootNode == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  syncState(renderer);

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    layoutGraphicMode(renderer);
  } else if (m_displayMode == BatteryDisplayMode::Glyph) {
    layoutGlyphMode(renderer, containerWidth, containerHeight);
  } else {
    layoutLabelOnlyMode(renderer, containerWidth, containerHeight);
  }
}

void BatteryWidget::layoutGraphicMode(Renderer& renderer) {
  auto* rootNode = root();
  if (m_bodyBg == nullptr || m_fillRect == nullptr || m_terminalNub == nullptr || rootNode == nullptr) {
    return;
  }

  const float scale = (Style::fontSizeBody / 14.0f) * m_contentScale;
  const float bodyW = std::round(kGraphicBodyWidth * scale);
  const float bodyH = std::round(kGraphicBodyHeight * scale);
  const float termW = std::round(kGraphicTerminalWidth * scale);
  const float termH = std::round(kGraphicTerminalHeight * scale);
  const float cornerR = std::round(kGraphicCornerRadius * scale);
  const float labelGap = std::round(Style::spaceXs * m_contentScale);
  const float stateGap = std::round(Style::spaceXs * 0.5f * m_contentScale);
  const bool showLabel = m_overlayLabel != nullptr && m_showLabel;
  const bool showStateGlyph = m_overlayGlyph != nullptr && m_overlayGlyph->visible();
  const bool hasOverlay = showLabel || showStateGlyph;

  if (showLabel) {
    m_overlayLabel->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_overlayLabel->measure(renderer);
  }
  if (showStateGlyph) {
    m_overlayGlyph->setGlyphSize(Style::fontSizeCaption * m_contentScale);
    m_overlayGlyph->measure(renderer);
  }

  if (m_isVertical) {
    const float graphicW = bodyH;
    const float graphicH = bodyW + termW;
    const float labelW = showLabel ? m_overlayLabel->width() : 0.0f;
    const float labelH = showLabel ? labelGap + m_overlayLabel->height() : 0.0f;
    const float stateW = showStateGlyph ? m_overlayGlyph->width() : 0.0f;
    const float stateH = showStateGlyph ? stateGap + m_overlayGlyph->height() : 0.0f;
    const float overlayGroupH = stateH + labelH;
    const float rootW = std::max({graphicW, labelW, stateW});
    const float bodyX = std::round((rootW - graphicW) * 0.5f);
    const float bodyY = termW;

    m_bodyBg->setRadius(cornerR);
    m_bodyBg->setPosition(bodyX, bodyY);
    m_bodyBg->setSize(bodyH, bodyW);

    m_terminalNub->setRadius(cornerR * 0.5f);
    m_terminalNub->setPosition(bodyX + std::round((bodyH - termH) * 0.5f), 0.0f);
    m_terminalNub->setSize(termH, termW);

    m_fillRect->setRadius(cornerR);
    updateFillGeometry();

    if (showStateGlyph) {
      m_overlayGlyph->setPosition(std::round((rootW - stateW) * 0.5f), graphicH + stateGap);
    }
    if (showLabel) {
      const float labelY = graphicH + labelGap + (showStateGlyph ? stateH : 0.0f);
      m_overlayLabel->setPosition(std::round((rootW - labelW) * 0.5f), labelY);
    }

    rootNode->setSize(rootW, graphicH + (hasOverlay ? overlayGroupH : 0.0f));
  } else {
    const float graphicW = bodyW + termW;
    const float graphicH = bodyH;
    const float labelW = showLabel ? labelGap + m_overlayLabel->width() : 0.0f;
    const float labelH = showLabel ? m_overlayLabel->height() : 0.0f;
    const float stateW = showStateGlyph ? stateGap + m_overlayGlyph->width() : 0.0f;
    const float stateH = showStateGlyph ? m_overlayGlyph->height() : 0.0f;
    const float overlayGroupW = stateW + labelW;
    const float overlayGroupH = std::max(labelH, stateH);
    const float rootH = std::max(graphicH, overlayGroupH);
    const float bodyY = std::round((rootH - bodyH) * 0.5f);

    m_bodyBg->setRadius(cornerR);
    m_bodyBg->setPosition(0.0f, bodyY);
    m_bodyBg->setSize(bodyW, bodyH);

    m_terminalNub->setRadius(cornerR * 0.5f);
    m_terminalNub->setPosition(bodyW, bodyY + std::round((bodyH - termH) * 0.5f));
    m_terminalNub->setSize(termW, termH);

    m_fillRect->setRadius(cornerR);
    updateFillGeometry();

    if (showStateGlyph) {
      m_overlayGlyph->setPosition(graphicW + stateGap, std::round((rootH - stateH) * 0.5f));
    }
    if (showLabel) {
      const float labelX = graphicW + labelGap + (showStateGlyph ? stateW : 0.0f);
      m_overlayLabel->setPosition(labelX, std::round((rootH - labelH) * 0.5f));
    }

    rootNode->setSize(graphicW + (hasOverlay ? overlayGroupW : 0.0f), rootH);
  }

  float rootW = rootNode->width();
  float rootH = rootNode->height();
  layoutBluetoothIndicator(renderer, rootW, rootH);
  rootNode->setSize(rootW, rootH);
}

void BatteryWidget::layoutGlyphMode(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }

  m_glyph->measure(renderer);

  const float hPad = m_hPadding * m_contentScale;

  if (m_label != nullptr && m_showLabel) {
    m_label->measure(renderer);

    if (m_isVertical) {
      const float w = std::max(m_glyph->width(), m_label->width());
      m_glyph->setPosition(hPad + std::round((w - m_glyph->width()) * 0.5f), 0.0f);
      m_label->setPosition(hPad + std::round((w - m_label->width()) * 0.5f), m_glyph->height());
      rootNode->setSize(w + 2.0f * hPad, m_glyph->height() + m_label->height());
    } else {
      const float h = std::max(m_glyph->height(), m_label->height());
      m_glyph->setPosition(hPad, std::round((h - m_glyph->height()) * 0.5f));
      m_label->setPosition(hPad + m_glyph->width() + Style::spaceXs, std::round((h - m_label->height()) * 0.5f));
      rootNode->setSize(m_label->x() + m_label->width() + hPad, h);
    }
  } else {
    m_glyph->setPosition(hPad, 0.0f);
    rootNode->setSize(m_glyph->width() + 2.0f * hPad, m_glyph->height());
  }

  float rootW = rootNode->width();
  float rootH = rootNode->height();
  layoutBluetoothIndicator(renderer, rootW, rootH);
  rootNode->setSize(rootW, rootH);
}

void BatteryWidget::layoutLabelOnlyMode(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_label == nullptr || rootNode == nullptr) {
    return;
  }

  m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
  m_label->measure(renderer);
  const float hPad = m_hPadding * m_contentScale;
  m_label->setPosition(hPad, 0.0f);
  rootNode->setSize(m_label->width() + 2.0f * hPad, m_label->height());

  float rootW = rootNode->width();
  float rootH = rootNode->height();
  layoutBluetoothIndicator(renderer, rootW, rootH);
  rootNode->setSize(rootW, rootH);
}

void BatteryWidget::layoutBluetoothIndicator(Renderer& renderer, float& rootWidth, float& rootHeight) {
  if (m_bluetoothGlyph == nullptr || !m_bluetoothGlyph->visible()) {
    return;
  }

  m_bluetoothGlyph->measure(renderer);

  const bool showLabel = m_bluetoothLabel != nullptr && m_bluetoothLabel->visible();
  if (showLabel) {
    m_bluetoothLabel->measure(renderer);
  }

  const float separatorGap = m_bluetoothGap * m_contentScale;
  const float innerGap = Style::spaceXs * m_contentScale;
  const float glyphW = m_bluetoothGlyph->width();
  const float glyphH = m_bluetoothGlyph->height();
  const float labelW = showLabel ? m_bluetoothLabel->width() : 0.0f;
  const float labelH = showLabel ? m_bluetoothLabel->height() : 0.0f;

  if (m_isVertical) {
    const float groupW = std::max(glyphW, labelW);
    const float groupH = glyphH + (showLabel ? innerGap + labelH : 0.0f);
    const float startY = rootHeight + separatorGap;
    const float startX = std::round((rootWidth - groupW) * 0.5f);
    m_bluetoothGlyph->setPosition(startX + std::round((groupW - glyphW) * 0.5f), startY);
    if (showLabel) {
      m_bluetoothLabel->setPosition(startX + std::round((groupW - labelW) * 0.5f), startY + glyphH + innerGap);
    }
    rootWidth = std::max(rootWidth, groupW);
    rootHeight = startY + groupH;
  } else {
    const float hPad = m_hPadding * m_contentScale;
    const float groupW = glyphW + (showLabel ? innerGap + labelW : 0.0f);
    const float groupH = std::max(glyphH, labelH);
    const float startX = rootWidth + separatorGap;
    const float groupY = std::round((rootHeight - groupH) * 0.5f);
    m_bluetoothGlyph->setPosition(startX, groupY + std::round((groupH - glyphH) * 0.5f));
    if (showLabel) {
      m_bluetoothLabel->setPosition(startX + glyphW + innerGap, groupY + std::round((groupH - labelH) * 0.5f));
    }
    rootWidth = startX + groupW + hPad;
    rootHeight = std::max(rootHeight, groupH);
  }
}

void BatteryWidget::syncBluetoothState(Renderer& renderer) {
  if (m_bluetoothGlyph == nullptr || m_bluetooth == nullptr) {
    return;
  }

  std::vector<int> percentages;
  bool anyConnected = false;
  const char* deviceGlyph = "bluetooth-device-generic";
  for (const auto& d : m_bluetooth->devices()) {
    if (!d.connected) {
      continue;
    }
    if (!anyConnected) {
      deviceGlyph = bluetoothDeviceGlyph(d.kind);
    }
    anyConnected = true;
    if (d.hasBattery) {
      percentages.push_back(d.batteryPercent);
    }
  }

  std::string text;
  for (std::size_t i = 0; i < percentages.size(); ++i) {
    if (i > 0) {
      text += " ";
    }
    text += std::to_string(percentages[i]) + "%";
  }

  const bool show = m_showBluetoothDevices && anyConnected;
  if (show == m_lastBluetoothVisible && text == m_lastBluetoothText) {
    return;
  }
  m_lastBluetoothVisible = show;
  m_lastBluetoothText = text;

  m_bluetoothGlyph->setVisible(show);
  if (m_bluetoothLabel != nullptr) {
    m_bluetoothLabel->setVisible(show && !text.empty());
  }
  if (!show) {
    requestRedraw();
    return;
  }

  m_bluetoothGlyph->setGlyph(deviceGlyph);
  m_bluetoothGlyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_bluetoothGlyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_bluetoothGlyph->measure(renderer);

  if (m_bluetoothLabel != nullptr && !text.empty()) {
    m_bluetoothLabel->setText(text);
    m_bluetoothLabel->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_bluetoothLabel->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_bluetoothLabel->measure(renderer);
  }

  requestRedraw();
}

void BatteryWidget::updateFillGeometry() {
  if (m_fillRect == nullptr || m_bodyBg == nullptr) {
    return;
  }

  const float fraction = std::clamp(m_animatedPct / 100.0f, 0.0f, 1.0f);

  if (m_isVertical) {
    const float bodyW = m_bodyBg->width();
    const float bodyH = m_bodyBg->height();
    const float fillH = bodyH * fraction;
    m_fillRect->setPosition(m_bodyBg->x(), m_bodyBg->y() + bodyH - fillH);
    m_fillRect->setSize(bodyW, fillH);
  } else {
    const float bodyW = m_bodyBg->width();
    const float bodyH = m_bodyBg->height();
    const float fillW = bodyW * fraction;
    m_fillRect->setPosition(m_bodyBg->x(), m_bodyBg->y());
    m_fillRect->setSize(fillW, bodyH);
  }
}

void BatteryWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void BatteryWidget::onFrameTick(float /*deltaMs*/) { requestRedraw(); }

bool BatteryWidget::needsFrameTick() const { return m_displayMode == BatteryDisplayMode::Graphic && m_fillAnim != 0; }

void BatteryWidget::syncState(Renderer& renderer) {
  if (m_upower == nullptr) {
    return;
  }

  syncBluetoothState(renderer);

  const auto s = m_upower->stateForDevice(m_deviceSelector);

  const auto now = std::chrono::steady_clock::now();
  const bool forceTimeRefresh = (m_lastTooltipRefreshTime == std::chrono::steady_clock::time_point{})
      || (now - m_lastTooltipRefreshTime >= std::chrono::seconds(15));

  if (s.percentage == m_lastPct
      && s.state == m_lastState
      && s.isPresent == m_lastPresent
      && s.energyRate == m_lastEnergyRate
      && s.timeToEmpty == m_lastTimeToEmpty
      && m_isVertical == m_lastVertical
      && !forceTimeRefresh) {
    return;
  }

  m_lastPct = s.percentage;
  m_lastState = s.state;
  m_lastPresent = s.isPresent;
  m_lastEnergyRate = s.energyRate;
  m_lastTimeToEmpty = s.timeToEmpty;
  m_lastVertical = m_isVertical;
  m_lastTooltipRefreshTime = now;

  const bool isPluggedIn = s.state == BatteryState::Charging
      || s.state == BatteryState::FullyCharged
      || s.state == BatteryState::PendingCharge;

  const bool hasVisibleContent = m_displayMode != BatteryDisplayMode::None || m_showLabel;

  const bool showWidget = s.isPresent
      && hasVisibleContent
      && !(m_hideWhenPlugged && isPluggedIn)
      && !(m_hideWhenFull && (s.state == BatteryState::FullyCharged || s.state == BatteryState::PendingCharge));

  auto* rootNode = root();
  if (rootNode != nullptr) {
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
  }

  if (!showWidget) {
    return;
  }

  const int pct = static_cast<int>(std::round(s.percentage));
  const bool isWarning = m_warningThreshold > 0 && pct <= m_warningThreshold && !isPluggedIn;

  if (m_displayMode == BatteryDisplayMode::Graphic) {
    const ColorSpec normalFgColor = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
    const ColorSpec fgColor = isWarning ? m_warningColor : normalFgColor;

    if (m_fillRect != nullptr) {
      m_fillRect->setFill(fgColor);
    }
    if (m_bodyBg != nullptr) {
      m_bodyBg->setFill(withOpacity(fgColor, 0.3f));
    }

    if (m_terminalNub != nullptr) {
      m_terminalNub->setFill(withOpacity(fgColor, 0.3f));
    }

    // Animate fill percentage
    const auto newPct = static_cast<float>(s.percentage);
    if (m_animations != nullptr && std::abs(m_animatedPct - newPct) > 0.5f) {
      m_animations->cancel(m_fillAnim);
      m_fillAnim = m_animations->animate(
          m_animatedPct, newPct, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
          [this](float v) {
            m_animatedPct = v;
            updateFillGeometry();
            requestRedraw();
          },
          [this]() { m_fillAnim = 0; }, this
      );
      requestFrameTick();
    } else {
      m_animatedPct = newPct;
      updateFillGeometry();
    }

    // Graphic mode label
    if (m_overlayLabel != nullptr && m_showLabel) {
      m_overlayLabel->setText(buildLabelText(pct, s));
      m_overlayLabel->setColor(fgColor);
      m_overlayLabel->measure(renderer);
    }

    // Overlay glyph — state icon
    const char* stateGlyph = batteryStateGlyph(s.state);
    if (m_overlayGlyph != nullptr) {
      if (stateGlyph != nullptr) {
        m_overlayGlyph->setGlyph(stateGlyph);
        m_overlayGlyph->setColor(fgColor);
        m_overlayGlyph->measure(renderer);
      }
    }

    if (m_overlayLabel != nullptr) {
      m_overlayLabel->setVisible(m_showLabel);
    }
    if (m_overlayGlyph != nullptr) {
      m_overlayGlyph->setVisible(stateGlyph != nullptr);
    }
  } else if (m_displayMode == BatteryDisplayMode::Glyph) {
    const ColorSpec normalFgColor = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface));
    const ColorSpec fgColor = isWarning ? m_warningColor : normalFgColor;

    if (m_glyph != nullptr) {
      m_glyph->setGlyph(batteryGlyphName(s.percentage, s.state));
      m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
      m_glyph->setColor(fgColor);
      m_glyph->measure(renderer);
    }

    if (m_label != nullptr && m_showLabel) {
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
      m_label->setText(buildLabelText(pct, s));
      m_label->setColor(fgColor);
      m_label->measure(renderer);
    }
  } else if (m_displayMode == BatteryDisplayMode::None) {
    const ColorSpec normalFgColor = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
    const ColorSpec fgColor = isWarning ? m_warningColor : normalFgColor;

    if (m_label != nullptr && m_showLabel) {
      m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
      m_label->setText(buildLabelText(pct, s));
      m_label->setColor(fgColor);
      m_label->measure(renderer);
    }
  }

  // Tooltip (both modes)
  if (rootNode != nullptr && m_tooltip) {
    auto devices = m_upower->batteryDevices();
    auto laptopEnd =
        std::ranges::stable_partition(devices, [](const UPowerDeviceInfo& d) { return d.isLaptopBattery(); }).begin();
    int laptopBatteryCount = static_cast<int>(laptopEnd - devices.begin());

    std::vector<TooltipRow> rows;
    int laptopBatteryIndex = 0;
    for (const auto& dev : devices) {
      std::string name;
      if (dev.isLaptopBattery()) {
        name = (laptopBatteryCount > 1)
            ? i18n::tr("power.battery.tooltip.device-numbered", "index", ++laptopBatteryIndex)
            : i18n::tr("power.battery.tooltip.device");
      } else {
        name = !dev.model.empty()
            ? dev.model
            : (!dev.nativePath.empty() ? dev.nativePath : i18n::tr("power.battery.tooltip.unknown-device"));
      }
      int dp = static_cast<int>(std::round(dev.state.percentage));
      rows.push_back({std::move(name), std::to_string(dp) + "%"});

      if (dev.isLaptopBattery()) {
        rows.push_back({i18n::tr("power.battery.tooltip.status"), batteryStateLabel(dev.state.state)});

        if (dev.state.timeToEmpty > 0) {
          auto dur = formatDuration(std::chrono::seconds(dev.state.timeToEmpty));
          rows.push_back({i18n::tr("power.battery.tooltip.time-left"), std::move(dur)});
        } else if (dev.state.timeToFull > 0) {
          auto dur = formatDuration(std::chrono::seconds(dev.state.timeToFull));
          rows.push_back({i18n::tr("power.battery.tooltip.time-to-full"), std::move(dur)});
        }

        if (dev.state.energyRate > 0.0) {
          std::ostringstream oss;
          oss << std::fixed;
          oss.precision(1);
          oss << dev.state.energyRate << " W";
          rows.push_back({i18n::tr("power.battery.tooltip.rate"), oss.str()});
        }

        if (dev.energyFullDesign > 0.0) {
          int health = static_cast<int>(std::round(dev.energyFull / dev.energyFullDesign * 100.0));
          rows.push_back({i18n::tr("power.battery.tooltip.health"), std::to_string(health) + "%"});
        }
      }
    }
    if (!rows.empty()) {
      static_cast<InputArea*>(rootNode)->setTooltip(std::move(rows));
    } else {
      static_cast<InputArea*>(rootNode)->clearTooltip();
    }
  } else if (rootNode != nullptr) {
    static_cast<InputArea*>(rootNode)->clearTooltip();
  }

  requestRedraw();
}
