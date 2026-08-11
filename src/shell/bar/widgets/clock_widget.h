#pragma once

#include "shell/bar/widget.h"

#include <cstdint>
#include <string>

class Label;
struct wl_output;

class ClockWidget : public Widget {
public:
  struct Options {
    std::string format = "{:%H:%M}";
    std::string verticalFormat;
    std::string tooltipFormat;
    std::string timezone;
    float hPadding = 4.0f;
    bool tooltip = true;
  };

  ClockWidget(wl_output* output, Options options);

  void create() override;

private:
  [[nodiscard]] std::string formatTimeText() const;
  [[nodiscard]] std::string formatDateText() const;
  [[nodiscard]] std::string formatTooltipText() const;
  void toggleDateDisplay();
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  std::string m_format;
  std::string m_verticalFormat;
  std::string m_tooltipFormat;
  std::string m_timezone;
  float m_hPadding = 4.0f;
  bool m_tooltip = true;
  bool m_isVertical = false;
  Label* m_label = nullptr;
  Label* m_secondaryLabel = nullptr;
  std::string m_lastText;
  std::string m_lastPrimaryText;
  std::string m_lastSecondaryText;
  std::string m_lastTooltipText;
  bool m_showingDate = false;
};
