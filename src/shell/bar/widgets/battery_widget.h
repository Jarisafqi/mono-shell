#pragma once

#include "dbus/upower/upower_service.h"
#include "render/animation/animation_manager.h"
#include "shell/bar/widget.h"
#include "ui/palette.h"

#include <chrono>
#include <string>

class Box;
class Glyph;
class Label;
class BluetoothService;

enum class BatteryDisplayMode : std::uint8_t { None, Graphic, Glyph };
enum class BatteryLabelContent : std::uint8_t { Percent, Time, Rate };

class BatteryWidget : public Widget {
public:
  struct Options {
    std::string deviceSelector = "auto";
    int warningThreshold = 0;
    ColorSpec warningColor = colorSpecFromRole(ColorRole::Error);
    BatteryDisplayMode displayMode = BatteryDisplayMode::Glyph;
    BatteryLabelContent labelContent = BatteryLabelContent::Percent;
    bool showLabel = true;
    bool hideWhenPlugged = false;
    bool hideWhenFull = false;
    bool showBluetoothDevices = true;
    float bluetoothGap = 4.0f;
    float hPadding = 4.0f;
    bool tooltip = true;
  };

  BatteryWidget(UPowerService* upower, BluetoothService* bluetooth, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool needsFrameTick() const override;
  void syncState(Renderer& renderer);
  void updateFillGeometry();
  [[nodiscard]] std::string buildLabelText(int pct, const UPowerState& state) const;

  void createGraphicMode();
  void createGlyphMode();
  void createLabelOnlyMode();
  void layoutGraphicMode(Renderer& renderer);
  void layoutGlyphMode(Renderer& renderer, float containerWidth, float containerHeight);
  void layoutLabelOnlyMode(Renderer& renderer, float containerWidth, float containerHeight);
  void layoutBluetoothIndicator(Renderer& renderer, float& rootWidth, float& rootHeight);
  void syncBluetoothState(Renderer& renderer);

  UPowerService* m_upower = nullptr;
  BluetoothService* m_bluetooth = nullptr;
  std::string m_deviceSelector = "auto";
  int m_warningThreshold = 0;
  ColorSpec m_warningColor;
  BatteryDisplayMode m_displayMode = BatteryDisplayMode::Glyph;
  BatteryLabelContent m_labelContent = BatteryLabelContent::Percent;
  bool m_showLabel = true;
  bool m_hideWhenPlugged = false;
  bool m_hideWhenFull = false;
  bool m_showBluetoothDevices = true;
  float m_bluetoothGap = 4.0f;
  float m_hPadding = 4.0f;
  bool m_tooltip = true;

  // Glyph mode nodes
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;

  // Graphic mode nodes
  Box* m_bodyBg = nullptr;
  Box* m_fillRect = nullptr;
  Box* m_terminalNub = nullptr;
  Label* m_overlayLabel = nullptr;
  Glyph* m_overlayGlyph = nullptr;

  // Bluetooth indicator nodes
  Glyph* m_bluetoothGlyph = nullptr;
  Label* m_bluetoothLabel = nullptr;
  bool m_lastBluetoothVisible = false;
  std::string m_lastBluetoothText;

  // Animated fill
  float m_animatedPct = 0.0f;
  AnimationManager::Id m_fillAnim = 0;

  double m_lastPct = -1.0;
  BatteryState m_lastState = BatteryState::Unknown;
  bool m_lastPresent = false;
  bool m_isVertical = false;
  bool m_lastVertical = false;

  double m_lastEnergyRate = -1.0;
  std::int64_t m_lastTimeToEmpty = -1;
  std::chrono::steady_clock::time_point m_lastTooltipRefreshTime;
};
