#include "theme/builtin_palettes.h"

#include "theme/fixed_palette.h"

#include <array>
#include <string_view>

namespace noctalia::theme {

  namespace {

    constexpr std::array<BuiltinPalette, 1> kPalettes =
        {
            {
                {
                    .name = "Black & White",
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#FFFFFF"),
                                    .onPrimary = hex("#000000"),
                                    .secondary = hex("#FFFFFF"),
                                    .onSecondary = hex("#000000"),
                                    .tertiary = hex("#FFFFFF"),
                                    .onTertiary = hex("#000000"),
                                    .error = hex("#FF5252"),
                                    .onError = hex("#000000"),
                                    .surface = hex("#000000"),
                                    .onSurface = hex("#FFFFFF"),
                                    .surfaceVariant = hex("#000000"),
                                    .onSurfaceVariant = hex("#FFFFFF"),
                                    .outline = hex("#FFFFFF"),
                                    .shadow = hex("#000000"),
                                    .hover = hex("#FFFFFF"),
                                    .onHover = hex("#000000"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#000000"),
                                            .red = hex("#FF5252"),
                                            .green = hex("#FFFFFF"),
                                            .yellow = hex("#FFFFFF"),
                                            .blue = hex("#FFFFFF"),
                                            .magenta = hex("#FFFFFF"),
                                            .cyan = hex("#FFFFFF"),
                                            .white = hex("#FFFFFF"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#000000"),
                                            .red = hex("#FF5252"),
                                            .green = hex("#FFFFFF"),
                                            .yellow = hex("#FFFFFF"),
                                            .blue = hex("#FFFFFF"),
                                            .magenta = hex("#FFFFFF"),
                                            .cyan = hex("#FFFFFF"),
                                            .white = hex("#FFFFFF"),
                                        },
                                    .foreground = hex("#FFFFFF"),
                                    .background = hex("#000000"),
                                    .selectionFg = hex("#000000"),
                                    .selectionBg = hex("#FFFFFF"),
                                    .cursorText = hex("#000000"),
                                    .cursor = hex("#FFFFFF"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                Palette{
                                    .primary = hex("#000000"),
                                    .onPrimary = hex("#FFFFFF"),
                                    .secondary = hex("#000000"),
                                    .onSecondary = hex("#FFFFFF"),
                                    .tertiary = hex("#000000"),
                                    .onTertiary = hex("#FFFFFF"),
                                    .error = hex("#FF5252"),
                                    .onError = hex("#FFFFFF"),
                                    .surface = hex("#FFFFFF"),
                                    .onSurface = hex("#000000"),
                                    .surfaceVariant = hex("#FFFFFF"),
                                    .onSurfaceVariant = hex("#000000"),
                                    .outline = hex("#000000"),
                                    .shadow = hex("#FFFFFF"),
                                    .hover = hex("#000000"),
                                    .onHover = hex("#FFFFFF"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#FFFFFF"),
                                            .red = hex("#FF5252"),
                                            .green = hex("#000000"),
                                            .yellow = hex("#000000"),
                                            .blue = hex("#000000"),
                                            .magenta = hex("#000000"),
                                            .cyan = hex("#000000"),
                                            .white = hex("#000000"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#FFFFFF"),
                                            .red = hex("#FF5252"),
                                            .green = hex("#000000"),
                                            .yellow = hex("#000000"),
                                            .blue = hex("#000000"),
                                            .magenta = hex("#000000"),
                                            .cyan = hex("#000000"),
                                            .white = hex("#000000"),
                                        },
                                    .foreground = hex("#000000"),
                                    .background = hex("#FFFFFF"),
                                    .selectionFg = hex("#FFFFFF"),
                                    .selectionBg = hex("#000000"),
                                    .cursorText = hex("#FFFFFF"),
                                    .cursor = hex("#000000"),
                                },
                        },
                },
            }
    };

  } // namespace

  std::span<const BuiltinPalette> builtinPalettes() { return kPalettes; }

  const BuiltinPalette* findBuiltinPalette(std::string_view name) {
    for (const auto& palette : kPalettes) {
      if (palette.name == name) {
        return &palette;
      }
    }
    return nullptr;
  }

  GeneratedPalette expandBuiltinPalette(const BuiltinPalette& palette) {
    auto generated = expandFixedPalettes(palette.dark.palette, palette.light.palette);
    applyTerminalPalette(generated.dark, palette.dark.terminal);
    applyTerminalPalette(generated.light, palette.light.terminal);
    return generated;
  }

} // namespace noctalia::theme