# Libretro core

The experimental libretro target embeds the magiceyes engine and uses the frontend for video,
audio and input. It does not link SDL. OpenGL ES titles use magiceyes' software rasterizer.

## Build

The core uses the same patched Unicorn checkout as the standalone engine:

```sh
make -f Makefile.libretro ME_UNICORN_FORK="$HOME/me-unicorn-fork"
```

This produces `magiceyes_libretro.so`. For a Windows cross-build, first build the Windows Unicorn
archive as described in `host/win/README.md`, then run:

```sh
make -f Makefile.libretro platform=win CC=x86_64-w64-mingw32-gcc
```

`CC` may be omitted when `x86_64-w64-mingw32-gcc` is on `PATH`.

Copy the resulting core (`.so`/`.dll`) into the frontend's cores directory and
`magiceyes_libretro.info` into its core-info directory.

## Runtime files

The core asks the frontend for its System and Save directories. Its layout is:

```text
<system>/magiceyes/
  rootfs/             optional OABI device runtime
  rootfs-eabi/        EABI runtime and guest shims
  overlay-oabi/       optional DRM/GLES guest overlays
  firmware/           staged device firmware

<save>/magiceyes/
  saves/              game-created files, separated by title
  cache/              GPEComp and zip extraction cache
  config/             future core configuration
```

Static GP2X games need no system files. As with the standalone build, firmware and games are not
distributed with magiceyes.

The initial core accepts `.gpe` and `.zip` by full path. Zip extraction currently uses the same
host `unzip`/`tar` path as the standalone engine, so raw `.gpe` content is recommended on hosts
without those tools.

## Current limitations

- The engine is asynchronous internally. Each `retro_run()` publishes the newest completed frame;
  a watchdog parks guest CPUs when the frontend stops calling it, but frame advance and
  fast-forward timing are not yet exact.
- Frontend save states are disabled. The native `.mst` implementation is file-oriented and its
  state size can grow after content load, which does not meet libretro's serialization contract.
- Rewind, runahead and deterministic netplay are consequently unsupported.
- The first supported targets are 64-bit desktop Linux and Windows.

## Smoke test

On Linux, the repository includes a tiny libretro host that checks content loading, RGB565 frame
delivery, PCM delivery and clean unload without requiring RetroArch:

```sh
tools/test/libretro_smoke.sh GAME.gpe SYSTEM_DIR SAVE_DIR [SECOND_GAME.gpe]
```

Set `ME_LIBRETRO_FAIL_FIRST=1`, `ME_LIBRETRO_PAUSE_TEST=1` or
`ME_LIBRETRO_RESET_TEST=1` to exercise failure recovery, frontend pausing or core reset.
