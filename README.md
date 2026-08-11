## Requirements

- Linux with a Wayland compositor (Hyprland, niri, mango, etc.)
- A C++23 toolchain and Meson
- [`just`](https://github.com/casey/just) (recommended) or plain Meson/ninja
- Fonts used by the default config (install beforehand on each machine):

  - **JetBrains Mono Slashed**
  - **OnePlus Sans Text**

  Other build dependencies (cairo, pango, pipewire, sdbus-c++, …) are picked up
  automatically via the Meson dependency system.

## Installation

### 1. Clone

```sh
git clone https://github.com/Jarisafqi/mono-shell
cd mono-shell
```

### 2. Configure and build

```sh
just configure release
just build release
```

Or with Meson directly:

```sh
meson setup build-release -Dbuildtype=release
meson compile -C build-release mono-shell
```

### 3. Install the shell

Install the binary and its assets system-wide (default prefix `/usr/local`):

```sh
sudo just install release
```

This runs `meson install` against the `release` build and places
`/usr/local/bin/mono-shell` plus the assets in `/usr/local/share/mono-shell`, so
the `mono-shell` command is available on your `PATH`.

Prefer a user prefix? Use:

```sh
meson install -C build-release --prefix "$HOME/.local"
```

### 4. Install the default config

The shipped `config.toml` is the portable default: it is free of
device-specific settings (display geometry, avatars, absolute paths), so the
shell looks and behaves the same on every machine.

```sh
just config-install
```

It is placed in `~/.config/mono-shell/config.toml` (honoring
`MONO_SHELL_CONFIG_HOME` / `XDG_CONFIG_HOME`), backing up any existing file.

The default config ships with the self-contained builtin **Black & White**
palette, so no external theme files are required.

### 5. Run

```sh
mono-shell
```

For development, `just run` builds and runs the debug binary directly.

## Cross-device consistency

Everything needed to keep several machines on the same shell is included:

| Command | Purpose |
| --- | --- |
| `just mono-pin` | record the source pin (commit + describe + dirty count) |
| `just mono-snapshot` | refresh `personal/` from the live config + Settings-UI state |
| `just mono-restore` | install the pinned `personal/` config/state on a machine |
| `just mono-check` | verify a machine matches the pin + snapshot (exits non-zero on drift) |

See `docs/device-consistency.md` for the full workflow.

## License

MIT — see [LICENSE](LICENSE). Built on the MIT-licensed [Noctalia shell](https://github.com/noctalia-dev/noctalia-shell) (Copyright 2025 noctalia-dev); fork maintained by Jarisafqi.
