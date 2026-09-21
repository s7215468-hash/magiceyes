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

The core accepts `.gpe` and `.zip` by full path. Zip files are extracted by the core's vendored
miniz implementation, with absolute paths and traversal entries rejected; no host `unzip` or
`tar` executable is needed.

## Core options

- **Timing model — Synchronized (default):** `retro_run()` grants the guest one 60 Hz slice and
  stops guest CPU execution before returning. Pausing or frame-advancing the frontend therefore
  does not let the guest run ahead. During frontend fast-forward, the guest clock and guest-side
  waits run at 4x and the CPU stays active between callbacks; a heartbeat watchdog still parks it
  if callbacks stop. Returning to normal speed restores the parked, one-slice-per-callback model.
- **Timing model — Asynchronous:** preserves the standalone engine's free-running behavior. A
  watchdog still parks the guest when frontend callbacks stop. This can be useful for diagnosing
  a title that depends on unusual host timing.
- **Pointer/touch input:** enables or disables mapping libretro pointer coordinates to the
  emulated touchscreen.

The reported geometry follows the active title if it changes resolution. The audio adapter clocks
44.1 kHz output against the 60 Hz frontend cadence, retains samples a frontend only partially
consumes, and keeps the guest-side mixer buffered instead of draining it in bursts.

## Save states

Frontend save and load state operations wrap the native `.mst` snapshot format. Libretro requires
one stable serialization size for the entire content session, so the core advertises a fixed
96 MiB buffer and rejects a snapshot if it ever exceeds that bound. The unused portion is cleared
for deterministic output. State files are staged briefly in `<save>/magiceyes/cache` because the
native snapshot implementation is transactional and file-oriented; the staging file is removed
after each operation and when content unloads.

## Current limitations

- Save states are large, so rewind and runahead have substantial memory and compression overhead.
- Deterministic netplay has not been validated.
- The first supported targets are 64-bit desktop Linux and Windows.

## Smoke test

On Linux, the repository includes a tiny libretro host that checks content loading, RGB565 frame
delivery, PCM delivery and clean unload without requiring RetroArch:

```sh
tools/test/libretro_smoke.sh GAME.gpe SYSTEM_DIR SAVE_DIR [SECOND_GAME.gpe]
```

Set `ME_LIBRETRO_FAIL_FIRST=1`, `ME_LIBRETRO_PAUSE_TEST=1`,
`ME_LIBRETRO_RESET_TEST=1`, `ME_LIBRETRO_STATE_TEST=1`,
`ME_LIBRETRO_PARTIAL_AUDIO=1`, `ME_LIBRETRO_FASTFORWARD=1`,
`ME_LIBRETRO_FASTFORWARD_TOGGLE=1` or `ME_LIBRETRO_ASYNC=1` to exercise failure recovery,
frontend pausing, core reset, state round-tripping, partial audio consumption, sustained or
on/off fast-forward, or asynchronous timing. `ME_LIBRETRO_RUNS=N` overrides the default 360
callbacks for longer pacing checks. The harness rejects audio batches or cumulative output above
the core's 44.1 kHz / 60 Hz budget.
