# Cross-device consistency

mono-shell is built from source, which means two machines only run the "same"
shell if **the same source produced the binary** and **the same config is
loaded**. Nothing else — distro, screen, theme — is copied automatically. This
directory + `tools/consistency.sh` make that reproducible.

Why machines drifted before

1. **Different binary version.** The build bakes
   `git describe --tags --always --dirty=-dirty --abbrev=12` into the binary
   (see `meson.build`), so `mono-shell --version` directly reveals which commit
   it came from. Two checkouts at different commits are two different shells,
   and a newer binary **auto-migrates an old config** (`config_version` in
   `src/config/config_migrations.cpp`), silently rewording/repositioning things.
2. **Different config.** All sizes/layout/behavior come from
   `$XDG_CONFIG_HOME/mono-shell/*.toml` plus Settings-UI overrides in
   `$XDG_STATE_HOME/mono-shell/settings.toml`. Those files are simply not
   synchronized between machines and drift independently.

The fix is to control both: pin the source, and snapshot the config+state, then
make every machine pass `mono-check`.

## What the tooling does

| File | Contents | Updated by |
| --- | --- | --- |
| `personal/config-merged.toml` | `mono-shell config export merged` (your choices, restorable) | `just mono-snapshot` |
| `personal/config-full.toml` | `mono-shell config export full` (effective config, drift-check "golden" file) | `just mono-snapshot` |
| `personal/state-settings.toml` | copy of `<state-dir>/settings.toml` (Settings-UI overrides) | `just mono-snapshot` |

`personal/mono-shell-version.txt` is **machine-local**: it is gitignored and
records this workspace's `commit` / `describe` / `dirty`. It cannot be tracked
in the same tree it pins — committing it would advance HEAD, so a rebuilt
binary would always bake a describe that postdates the pin. Cross-machine
builds instead agree on a **release tag**: devices check out the tag, build a
clean tree, and `mono-pin` records that clone's own local pin.

`state.toml` (runtime app state) is intentionally not snapshotted — it is
per-machine state, not configuration. `snapshot` and `check` also **scrub
device-scoped Settings-UI state**: lockscreen-widget geometry (`output`,
`cx`/`cy`, widget entries per display) is dropped before comparing, so
hotplugging or changing monitors never causes false drift. Your machine keeps
its own geometry locally; only the portable projection is shared.

## One-time setup (on the reference machine)

1. Make the checkout clean so the pinned build is reproducible:

   ```sh
   just mono-pin        # records the source pin
   ```

2. Snapshot the config you want every machine to share:

   ```sh
   just mono-snapshot
   ```

3. Commit `personal/` and the tooling:

   ```sh
   git add tools/consistency.sh docs/device-consistency.md personal/ justfile
   git commit -m "personal: pin mono-shell build + snapshot config"
   ```

   If `mono-check` still warns about a dirty tree after committing, re-run
   `just mono-pin` so the pin matches the clean commit.

## Setting up another machine

1. Clone the repo **at a release tag** and build with a clean tree:

   ```sh
   git checkout <release-tag>     # e.g. v0.1.0; see the README
   just mono-pin                  # records this clone's local pin (commit/describe)
   just configure release
   just build release
   ```

   `mono-pin` is safe to run here: it writes the gitignored local pin and leaves
   the tree clean.
   (If this release omits the `justfile` recipes, run the commands directly.)

2. Install the pinned config + state:

   ```sh
   just mono-restore
   ```

   `mono-restore` validates the snapshot with `config validate`, refuses to run
   against a binary that doesn't match the pin (so a newer/older build can't
   trigger config migrations), backs up any existing `*.toml`, and installs the
   snapshot.

3. Verify before first use:

   ```sh
   just mono-check      # exits non-zero on any drift
   ```

## After changing settings

```sh
just mono-snapshot      # refresh personal/ from the live config
# review the diff, then commit personal/
```

`mono-check` will now fail until you commit the fresh snapshot — that is the
intended guard so a machine is never silently out of date.

## Making sizes/layout deterministic

Copy these explicit values into the shared `config.toml` instead of leaving
them to per-machine defaults:

- `[accessibility] ui_scale` — fixed number (e.g. `1.0`), identical everywhere.
- `[shell] font_family` — a font installed on every machine; otherwise a
  missing font silently falls back.
- `[shell.animation] enabled` / `speed` — be explicit (e.g. `false` / `1.0`).
- `[shell.panel]` `*_placement`, `*_position` — fixed so layout is identical.
- `[theme]` `mode` (`"dark"`/`"light"`/`"auto"`) and `source`
  (`"builtin"` is byte-identical everywhere; `"wallpaper"`/`"community"` vary
  per machine and should not be used for consistency).

## Values that should stay machine-specific

Do **not** bake these into the shared snapshot — keep them as local overrides on
each machine or via a separate file:

- `[shell] avatar_path`
- wallpaper paths (mono-shell's own `[wallpaper]`/`[backdrop]`, plus any external
  WM-managed wallpaper)
- `[plugins.sources]` git/path sources and `plugin_data`
- anything per-output/per-HiDPI scaling beyond the shared `ui_scale`

## Notes and limitations

- A **dirty** pin (`describe` ends in `-dirty`, `dirty` in the pin file is > 0)
  means the binary was built from an uncommitted tree. It is only reproducible
  if every machine rebuilds that exact dirty tree. For true consistency, commit
  first and pin a clean commit.
- `check` compares the live `config export full` byte-for-byte against
  `personal/config-full.toml`, and state settings likewise; it also compares
  the discovered binary against the pin.
- The tooling honors `MONO_SHELL_BIN`,
  `MONO_SHELL_CONFIG_HOME` / `MONO_SHELL_STATE_HOME` (and the XDG equivalents),
  so it works without touching your real config (use throwaway dirs to test).