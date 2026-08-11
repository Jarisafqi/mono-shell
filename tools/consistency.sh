#!/usr/bin/env -S bash
#
# mono-shell cross-device consistency tooling.
#
# Makes a mono-shell install reproducible across machines by pinning the exact
# source the binary was built from and by snapshotting the config + Settings-UI
# state. Two machines that pass `check` against the same committed personal/
# directory run the same binary version with the same effective config.
#
# Usage:
#   tools/consistency.sh pin                                    Record source pin
#   tools/consistency.sh snapshot                               Refresh personal/ config + state
#   tools/consistency.sh restore [config-dir] [state-dir]       Install personal/ onto this machine
#   tools/consistency.sh check                                  Verify this machine matches the pin + snapshot
#
# The optional restore arguments are the mono-shell config/state directories
# (defaults mirror FileUtils: MONO_SHELL_CONFIG_HOME/XDG_CONFIG_HOME/+
# "mono-shell", state likewise). Set MONO_SHELL_BIN to force a binary path;
# otherwise build-release/build-debug/build are tried, then $PATH.
#
# Files under personal/:
#   mono-shell-version.txt  commit/describe/dirty recorded by `pin`
#   config-merged.toml      mono-shell config export merged (restorable config)
#   config-full.toml        mono-shell config export full (golden drift-check snapshot)
#   state-settings.toml     copy of <state-dir>/settings.toml (Settings-UI overrides)
#
# NOTE: the build bakes `git describe --tags --always --dirty=-dirty --abbrev=12`
# into the binary (see meson.build), so a pinned commit + clean tree is the only
# condition under which two machines produce an identical binary.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PERSONAL="$ROOT/personal"
PIN_FILE="$PERSONAL/mono-shell-version.txt"


log() { printf '[mono-consistency] %s\n' "$*"; }
fatal() { printf '[mono-consistency] error: %s\n' "$*" >&2; exit 1; }

find_bin() {
  if [[ -n "${MONO_SHELL_BIN:-}" ]]; then
    [[ -x "$MONO_SHELL_BIN" ]] || fatal "MONO_SHELL_BIN is not executable: $MONO_SHELL_BIN"
    printf '%s\n' "$MONO_SHELL_BIN"
    return
  fi
  for candidate in "$ROOT/build-release/mono-shell" "$ROOT/build-debug/mono-shell" "$ROOT/build/mono-shell"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done
  if command -v mono-shell >/dev/null 2>&1; then
    command -v mono-shell
    return
  fi
  return 1
}

resolve_config_dir() {
  if [[ -n "${MONO_SHELL_CONFIG_HOME:-}" ]]; then printf '%s/mono-shell' "$MONO_SHELL_CONFIG_HOME"; return; fi
  if [[ -n "${XDG_CONFIG_HOME:-}" ]]; then printf '%s/mono-shell' "$XDG_CONFIG_HOME"; return; fi
  printf '%s/.config/mono-shell' "$HOME"
}

resolve_state_dir() {
  local base
  if [[ -n "${MONO_SHELL_STATE_HOME:-}" ]]; then base="$MONO_SHELL_STATE_HOME";
  elif [[ -n "${XDG_STATE_HOME:-}" ]]; then base="$XDG_STATE_HOME";
  else base="$HOME/.local/state"; fi
  printf '%s/mono-shell' "$base"
}

git_describe() {
  git -C "$ROOT" describe --tags --always --dirty=-dirty --abbrev=12
}

git_commit() {
  git -C "$ROOT" rev-parse HEAD
}

git_dirty_count() {
  [[ -d "$ROOT/.git" ]] || return 0
  git -C "$ROOT" status --porcelain | grep -vc '^??' || true
}

# Extract the git describe string embedded in a built binary via --version,
# e.g. "mono-shell v5.0.0 (7855475588c5-dirty)" -> "7855475588c5-dirty".
binary_describe() {
  local raw
  raw="$("$1" --version)" || return 1
  local desc="${raw##*\(}"
  desc="${desc%\)*}"
  [[ "$desc" != "$raw" ]] || return 1
  printf '%s\n' "$desc"
}

# Run a mono-shell CLI subcommand against the live config/state directories.
run_live() {
  "$(find_bin)" config "$@"
}

export_full_live() {
  run_live export full
}

# Remove device-scoped state from a config/settings export so the snapshot stays
# portable: lockscreen widgets are positioned per-output (e.g.
# [lockscreen_widgets.widget."lockscreen-login-box@HDMI-A-1"]) and change
# whenever a display is hotplugged. Dropping the section makes the comparison
# insensitive to hardware; lockscreen widgets are off in the default config.
scrub_device_state() {
  awk '
    /^\[lockscreen_widgets/ { skip = 1; next }
    skip && /^\[/ { skip = 0 }
    skip { next }
    { print }
  '
}

source_describe() {
  git_describe
}

require_pin() {
  [[ -f "$PIN_FILE" ]] || fatal "no pin found ($PIN_FILE); run 'tools/consistency.sh pin' first"
}

cmd_pin() {
  [[ -d "$ROOT/.git" ]] || fatal "not a git checkout: $ROOT"
  local commit describe dirty
  commit="$(git_commit)"
  describe="$(source_describe)"
  dirty="$(git_dirty_count)"
  mkdir -p "$PERSONAL"
  cat > "$PIN_FILE" <<EOF
commit: $commit
describe: $describe
dirty: $dirty
EOF
  log "pinned $describe ($dirty uncommitted file(s)); rebuild with a clean tree for identical binaries"
}

cmd_snapshot() {
  local bin live_dirty
  bin="$(find_bin)" || fatal "no mono-shell binary found (set MONO_SHELL_BIN or build first)"
  log "using $bin ($("$bin" --version))"

  local merged full
  merged="$(run_live export merged)"
  full="$(export_full_live)"
  [[ -n "$(printf '%s' "$merged" | tr -d '[:space:]')" ]] || fatal "merged config export is empty"
  [[ -n "$(printf '%s' "$full" | tr -d '[:space:]')" ]] || fatal "effective config export is empty"

  mkdir -p "$PERSONAL"
  printf '%s\n' "$merged" | scrub_device_state > "$PERSONAL/config-merged.toml"
  printf '%s\n' "$full" | scrub_device_state > "$PERSONAL/config-full.toml"

  if ! run_live validate "$PERSONAL/config-merged.toml" >/dev/null 2>&1; then
    fatal "snapshot would not validate; fix the config or the snapshot source"
  fi

  local state settings
  state="$(resolve_state_dir)"
  settings="$state/settings.toml"
  if [[ -f "$settings" ]]; then
    scrub_device_state < "$settings" > "$PERSONAL/state-settings.toml"
    log "captured state settings ($settings, device-scoped keys scrubbed)"
  else
    rm -f "$PERSONAL/state-settings.toml"
    log "no state settings overrides present; snapshot records 'none'"
  fi

  log "snapshot written to $PERSONAL (config-merged.toml, config-full.toml, state-settings.toml)"
  log "re-run 'tools/consistency.sh pin' if the source changed, then commit personal/"
}

verify_pinned_binary() {
  local bin desc
  bin="$(find_bin)" || return 1
  desc="$(binary_describe "$bin")" || return 1
  local pin
  pin="$(sed -n 's/^describe: //p' "$PIN_FILE")"
  if [[ "$desc" != "$pin" ]]; then
    fatal "binary $bin reports $desc but the pin expects $pin; rebuild the pinned commit with a clean tree"
  fi
  return 0
}

cmd_restore() {
  local target_config target_state
  target_config="${1:-}"
  target_state="${2:-}"
  if [[ -z "$target_config" ]]; then target_config="$(resolve_config_dir)"; fi
  if [[ -z "$target_state" ]]; then target_state="$(resolve_state_dir)"; fi

  require_pin
  verify_pinned_binary || fatal "restore requires a binary matching the pin (migration safety)"
  [[ -f "$PERSONAL/config-merged.toml" ]] || fatal "missing $PERSONAL/config-merged.toml; run 'snapshot' first"

  log "validating snapshot config..."
  if ! "$(find_bin)" config validate "$PERSONAL/config-merged.toml" >/dev/null 2>&1; then
    fatal "snapshot config failed validation"
  fi

  local stamp backup_config
  stamp="$(date +%s)"
  backup_config="$target_config.pre-mono-restore.$stamp"

  mkdir -p "$target_config" "$target_state"

  log "backing up existing config tomls to $backup_config"
  mkdir -p "$backup_config"
  find "$target_config" -maxdepth 1 -name '*.toml' -exec mv -t "$backup_config" {} + 2>/dev/null || true

  cp "$PERSONAL/config-merged.toml" "$target_config/config.toml"
  log "installed $PERSONAL/config-merged.toml -> $target_config/config.toml"

  if [[ -f "$PERSONAL/state-settings.toml" ]]; then
    if [[ -f "$target_state/settings.toml" ]]; then
      mkdir -p "$backup_config"
      mv "$target_state/settings.toml" "$backup_config/settings.toml"
    fi
    cp "$PERSONAL/state-settings.toml" "$target_state/settings.toml"
    log "installed state settings -> $target_state/settings.toml"
  else
    if [[ -f "$target_state/settings.toml" ]]; then
      mkdir -p "$backup_config"
      mv "$target_state/settings.toml" "$backup_config/settings.toml"
    fi
    log "snapshot has no state settings; removed any local overrides"
  fi

  log "re-validating installed config..."
  if ! "$(find_bin)" config validate "$target_config" >/dev/null 2>&1; then
    fatal "installed config failed validation (restored files are in place)"
  fi
  log "restore complete; run 'tools/consistency.sh check' to confirm"
}

check_result=0

check_fail() { log "FAIL: $*"; check_result=1; }
check_warn() { log "WARN: $*"; }

check_build_pin() {
  require_pin
  local pinned_commit pinned_describe pinned_dirty
  pinned_commit="$(sed -n 's/^commit: //p' "$PIN_FILE")"
  pinned_describe="$(sed -n 's/^describe: //p' "$PIN_FILE")"
  pinned_dirty="$(sed -n 's/^dirty: //p' "$PIN_FILE")"

  if [[ -d "$ROOT/.git" ]]; then
    local now_commit now_describe now_dirty
    now_commit="$(git_commit)"
    now_describe="$(source_describe)"
    now_dirty="$(git_dirty_count)"
    if [[ "$now_commit" != "$pinned_commit" || "$now_describe" != "$pinned_describe" ]]; then
      check_fail "source is at $now_describe but the pin expects $pinned_describe"
    fi
    if [[ "$now_dirty" != "$pinned_dirty" ]]; then
      check_warn "worktree dirty-file count changed ($pinned_dirty -> $now_dirty); re-pin after committing"
    fi
  else
    check_warn "not a git checkout; skipping source pin comparison"
  fi

  local bin
  if bin="$(find_bin)"; then
    local desc
    if desc="$(binary_describe "$bin")"; then
      if [[ "$desc" != "$pinned_describe" ]]; then
        check_fail "binary $bin is $desc but the pin expects $pinned_describe (rebuild the pinned commit)"
      else
        log "binary $bin matches the pin ($desc)"
      fi
    else
      check_warn "could not extract a git revision from $bin --version"
    fi
  else
    check_warn "no mono-shell binary found; build the pinned commit and re-check"
  fi
}

check_config() {
  [[ -f "$PERSONAL/config-full.toml" ]] || { check_fail "missing $PERSONAL/config-full.toml; run 'snapshot'"; return; }

  local live_full
  if ! live_full="$(export_full_live 2>/dev/null)"; then
    check_fail "could not export the live effective config"
    return
  fi

  # Command substitution strips trailing newlines; normalize before comparing.
  local tmp
  tmp="$(mktemp)"
  printf '%s\n' "$live_full" | scrub_device_state > "$tmp"
  if ! cmp -s "$tmp" "$PERSONAL/config-full.toml"; then
    rm -f "$tmp"
    check_fail "live effective config differs from the snapshot; run 'snapshot' and commit the change"
    return
  fi
  rm -f "$tmp"
  log "effective config matches the snapshot"

  local state settings
  state="$(resolve_state_dir)"
  settings="$state/settings.toml"
  if [[ -f "$PERSONAL/state-settings.toml" ]]; then
    if [[ ! -f "$settings" ]]; then
      check_fail "snapshot records state settings but this machine has none"
    else
      local live_settings
      live_settings="$(scrub_device_state < "$settings")"
      if [[ "$live_settings" != "$(cat "$PERSONAL/state-settings.toml")" ]]; then
        check_fail "state settings differ from the snapshot; run 'snapshot' and commit the change"
      else
        log "state settings match the snapshot"
      fi
    fi
  else
    if [[ -f "$settings" ]]; then
      check_fail "this machine has state settings but the snapshot records none"
    else
      log "state settings match the snapshot (none)"
    fi
  fi
}

cmd_check() {
  check_build_pin
  check_config
  if [[ "$check_result" -eq 0 ]]; then
    log "this machine is consistent with the pinned personal/ snapshot"
  else
    log "drift detected; resolve the FAILs above before using this machine"
    exit 1
  fi
}

main() {
  local cmd="${1:-}"
  shift || true
  case "$cmd" in
    pin) cmd_pin ;;
    snapshot) cmd_snapshot ;;
    restore) cmd_restore "$@" ;;
    check) cmd_check ;;
    *)
      awk '
        NR <= 1 { next }
        /^#/    { hdr = 1; sub(/^# ?/, ""); print; next }
        hdr && /^[[:space:]]*$/ { print ""; next }
        hdr     { exit }
      ' "$0"
      return 1
      ;;
  esac
}

main "$@"