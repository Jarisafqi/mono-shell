#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Glyph names resolve in this order:
// 1. Explicit codepoint literals such as U+F123 or 0xF123.
// 2. Hand-curated Noctalia aliases from glyph_registry.cpp.
// 3. Native Material Design Icon names from assets/fonts/material-icons.json.
namespace GlyphRegistry {

  struct MaterialGlyphMetadata {
    char32_t codepoint = 0;
    std::string category;
  };

  [[nodiscard]] bool contains(std::string_view name);
  [[nodiscard]] char32_t lookup(std::string_view name);

  // Full Material icon catalog with structured metadata.
  [[nodiscard]] const std::unordered_map<std::string, MaterialGlyphMetadata>& materialGlyphMetadata();
  // Full Material icon catalog (loaded from assets/fonts/material-icons.json on first registry use).
  [[nodiscard]] const std::unordered_map<std::string, char32_t>& materialIcons();
  [[nodiscard]] std::optional<std::string_view> categoryFor(std::string_view name);
  // Hand-curated Noctalia alias -> native Material Design Icon name map.
  [[nodiscard]] const std::unordered_map<std::string, std::string_view>& aliases();

} // namespace GlyphRegistry
