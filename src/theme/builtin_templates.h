#pragma once

#include <string>
#include <vector>

namespace noctalia::theme {

  struct AvailableTemplate {
    std::string id;          // canonical TOML value (what gets written to config)
    std::string displayName; // friendly label for the GUI; falls back to id when not provided
    std::string category;
    std::vector<std::string> outputPaths;
    bool outputDynamic = false;
  };

} // namespace noctalia::theme
