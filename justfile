set positional-arguments

mode := "debug"
build-dir := "build-" + mode
prefix := "/usr/local"
cpp-std := "c++23"

default:
    @just --list

configure m=mode install_prefix=prefix:
    #!/usr/bin/env bash
    set -euo pipefail
    args=(--buildtype={{ if m == "release" { "release" } else { "debug" } }} -Dcpp_std={{cpp-std}} -Dtests=auto)
    [[ "{{m}}" == "release" ]] && args+=(-Db_lto=true)
    [[ "{{m}}" == "asan"    ]] && args+=(-Db_sanitize=address,undefined)
    if [[ -d "build-{{m}}" ]]; then
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}" --reconfigure
    else
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}"
    fi
    ln -sfn "build-{{m}}/compile_commands.json" compile_commands.json

build m=mode: (_ensure-configured m)
    meson compile -C build-{{m}} mono-shell

_ensure-configured m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        just configure {{m}}
        exit 0
    fi
    configure_output="$(meson configure "build-{{m}}")"
    current_cpp_std="$(awk '$1 == "cpp_std" { print $2; found=1 } END { if (!found) exit 1 }' <<<"$configure_output")"
    current_tests="$(awk '$1 == "tests" { print $2; found=1 } END { if (!found) exit 1 }' <<<"$configure_output")"
    args=()
    [[ "$current_cpp_std" != "{{cpp-std}}" ]] && args+=(-Dcpp_std={{cpp-std}})
    [[ "$current_tests" != "auto" ]] && args+=(-Dtests=auto)
    if (( ${#args[@]} > 0 )); then
        meson configure "build-{{m}}" "${args[@]}"
    fi

run m=mode: (build m)
    ./build-{{m}}/mono-shell

# Record the source pin (commit + git-describe + dirty count) that defines a
# consistent cross-device build. Re-run after committing, and rebuild with a
# clean tree so the baked-in git revision is identical everywhere.
mono-pin:
    ./tools/consistency.sh pin

# Snapshot the live config + Settings-UI state into personal/. Run after
# changing settings, then commit personal/.
mono-snapshot:
    ./tools/consistency.sh snapshot

# Restore the pinned personal/ config + state onto this machine.
# Optional args: <config-dir> <state-dir> (defaults follow the XDG/MONO_SHELL_* env).
mono-restore *args:
    ./tools/consistency.sh restore {{args}}

# Verify this machine matches the pinned build + snapshot. Exits non-zero on
# drift so an out-of-date machine is caught before use.
mono-check:
    ./tools/consistency.sh check

# Install the portable default config.toml into the user's config dir, backing
# up any existing file first. Target mirrors FileUtils:
# $MONO_SHELL_CONFIG_HOME || $XDG_CONFIG_HOME/mono-shell || ~/.config/mono-shell.
# Validates the config first when a mono-shell binary is available.
config-install:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f config.toml ]]; then
        echo "error: config.toml not found in the repo root" >&2
        exit 1
    fi
    if command -v mono-shell >/dev/null 2>&1 && ! mono-shell config validate config.toml >/dev/null 2>&1; then
        echo "error: config.toml failed validation" >&2
        exit 1
    fi
    conf_dir="${MONO_SHELL_CONFIG_HOME:-${XDG_CONFIG_HOME:-$HOME/.config}}/mono-shell"
    mkdir -p "$conf_dir"
    if [[ -f "$conf_dir/config.toml" && ! -f "$conf_dir/config.toml.bak" ]]; then
        cp -v "$conf_dir/config.toml" "$conf_dir/config.toml.bak"
    fi
    cp -v config.toml "$conf_dir/config.toml"

# Build and run the unit tests, enabling their targets when auto mode omits them.
test m=mode *args: (_ensure-configured m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "{{m}}" == "release" || "{{m}}" == "asan" ]]; then
        meson setup "build-{{m}}" -Dtests=enabled --reconfigure >/dev/null
    fi
    meson test -C build-{{m}} {{args}}

install m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -x "build-{{m}}/mono-shell" ]]; then
        echo "error: build-{{m}}/mono-shell is missing; run 'just build {{m}}' before installing" >&2
        exit 1
    fi
    meson install --no-rebuild -C build-{{m}}

uninstall m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        echo "error: build-{{m}} is missing or was not configured with the Ninja backend; nothing to uninstall" >&2
        exit 1
    fi
    ninja -C build-{{m}} uninstall

format:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

_clang_tidy m=mode *args:
    #!/usr/bin/env bash
    set -euo pipefail
    src_root="$(realpath src)"
    # compile_commands.json stores build-relative paths, so clang-tidy emits header
    # diagnostics as ../src/...; the header-filter must match that form (an absolute
    # ^${src_root} anchor never matches, silently dropping every header diagnostic).
    # ../src/ also excludes vendored third_party/*/src/* headers.
    run-clang-tidy -quiet -use-color -p "build-{{m}}" -j "$(nproc)" -header-filter='\.\./src/.*' {{args}} "^${src_root}/.*"

lint m=mode: (_ensure-configured m)
    just _clang_tidy {{m}} '-warnings-as-errors=*'

fix m=mode: (_ensure-configured m)
    just _clang_tidy {{m}} -fix
    just format

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
