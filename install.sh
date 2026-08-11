#!/usr/bin/env bash
#
# One-shot installer for mono-shell.
#
# Clones the repo (if needed), configures, builds, installs the binary and
# assets, and deploys the default config. Run from anywhere:
#
#     bash <(curl -fsSL https://raw.githubusercontent.com/Jarisafqi/mono-shell/main/install.sh)
#
# or:
#
#     ./install.sh [--prefix DIR]
#
# Options:
#   --prefix DIR   install under DIR instead of /usr/local (no sudo needed)
#
set -euo pipefail

REPO_URL="https://github.com/Jarisafqi/mono-shell.git"
REPO_DIR="$(pwd)/mono-shell"
PREFIX="/usr/local"
USE_SUDO="yes"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)
      PREFIX="$2"
      USE_SUDO="no"
      shift 2
      ;;
    *)
      echo "unknown option: $1" >&2
      exit 1
      ;;
  esac
done

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

require() {
  for cmd in "$@"; do
    command -v "$cmd" >/dev/null 2>&1 || die "missing dependency: $cmd"
  done
}

require git meson ninja

# 1. Clone
if [[ ! -d "$REPO_DIR/.git" ]]; then
  say "Cloning mono-shell into $REPO_DIR"
  git clone --depth 1 "$REPO_URL" "$REPO_DIR"
else
  say "Repo already present at $REPO_DIR; pulling latest"
  git -C "$REPO_DIR" pull --ff-only
fi
cd "$REPO_DIR"

# 2. Configure and build
say "Configuring release build (prefix: $PREFIX)"
meson setup build-release -Dbuildtype=release -Dcpp_std=c++23 -Dtests=auto \
  -Db_lto=true --prefix "$PREFIX"
say "Building mono-shell"
meson compile -C build-release mono-shell

# 3. Install
if [[ "$USE_SUDO" == "yes" ]]; then
  say "Installing to $PREFIX (may ask for your password)"
  sudo meson install -C build-release --no-rebuild
else
  say "Installing to $PREFIX"
  meson install -C build-release --no-rebuild
fi

# 4. Default config
say "Installing the default config"
install -Dm644 -t "$(printf '%s/mono-shell' "${MONO_SHELL_CONFIG_HOME:-${XDG_CONFIG_HOME:-$HOME/.config}}")" \
  config.toml

say "Done. Run 'mono-shell' (or restart your compositor)."
