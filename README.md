> **Disclaimer:** this library was extracted from Project Midas with AI assistance ([Claude Code](https://claude.com/claude-code)).

# Midas Raphnet RAW Driver

The raphnet N64-to-USB adapter driver from **Project Midas** (the GoldenEye 007
PC port), as a small C library for other PC decomps and recomps.

| Part | What it gives you |
|---|---|
| **RAW driver** (`mf_raphnet`) | A real N64 controller on a raphnet adapter, read straight from the adapter's vendor HID interface: the exact 4 bytes a console sees, with no OS joystick scaling. Background poller, hotplug and reconnect, single- and dual-port adapters, firmware before and after v3.4. |
| **Rumble Pak** | A real Rumble Pak, driven through the pak protocol itself, so it needs no firmware support. |
| **Probe tool** (`mf_raphnet_probe`) | A live readout of both channels, to check an adapter works before wiring it into a game. |

Plain C99, one source file. It uses Win32 HID on Windows (no SDL) and SDL2
elsewhere.

For the mouse, the right-stick look and modern-pad stick shaping, see the
companion
[Midas-Mouse-and-Look](https://github.com/murkantor/Midas-Mouse-and-Look).

## Add it to your game with one prompt

In Claude Code (or another AI coding agent), from your game's repository:

```
Add https://github.com/murkantor/Midas-Raphnet-Raw-Driver to this project. Follow its CLAUDE.md.
```

[CLAUDE.md](CLAUDE.md) tells the agent how to vendor the driver, find where your
port builds its pad state, keep the raphnet stick untouched, and hide the
adapter's duplicate joystick. It also lists what you need to test with a real
adapter.

## Why it feels right

Players praise how a real N64 controller feels in Project Midas. The game gets
**the controller's real poll response**, with nothing in between:

- **No OS joystick layer.** It uses the raw SI get-status command. On Windows,
  the normal joystick path applies the OS's own per-device calibration and
  scaling, so its values are neither the real stick nor the same on two
  machines.
- **Zero translation.** Buttons arrive in the N64 `CONT_*` bit layout, and the
  stick arrives as signed N64 units. The game's own deadzone and curve then run
  exactly as on the console.
- **No frame-rate cost.** One USB exchange takes about 5 ms however tight the
  loop is. Done on the frame thread, it capped Project Midas at about 140 fps.
  Polling runs on a background thread that never spins, and the game reads a
  lock-free cache.
- **No lag from idle ports or rumble.** Empty ports drop to a 4 Hz rescan, and
  rumble is applied after each read as desired state, so neither delays the pad
  being played.
- **Self-healing.** A stale handle is reopened at most once a second, and a
  replugged adapter comes back without a restart.

## Docs

- **[docs/INTEGRATION.md](docs/INTEGRATION.md)** is step-by-step merging into
  other ports: N64 decomps and recomps, and N64 pads in GameCube, PS2 and Xbox
  ports, with the pitfalls for each.
- **[docs/PROVENANCE.md](docs/PROVENANCE.md)** maps every function back to the
  Project Midas code it came from.

## Build

```bash
cmake -S . -B build
cmake --build build
./build/mf_raphnet_probe          # live readout of both channels
./build/mf_raphnet_probe rumble   # also pulses the Rumble Pak on channel 0
```

As a subproject:

```cmake
add_subdirectory(third_party/raphnet-raw-driver)
target_link_libraries(mygame PRIVATE midas_feel_raphnet)
```

Or add `src/mf_raphnet.c` to your build and `include/` to your include path.

| Host | Transport | Needs |
|---|---|---|
| Windows | native SetupDi + `HidD_*` (SDL's hidapi *filters out* the vendor interface on Windows, so SDL isn't used) | links `hid`, `setupapi` |
| Linux / macOS | SDL2 ≥ 2.0.18 `SDL_hid_*` | SDL2; on Linux, hidraw access (below) |
| SDL3 | `-DRAPHNET_RAW_SDL3=ON` | **not yet compiled or tested** |

**Linux permissions.** Add `/etc/udev/rules.d/70-raphnet.rules`:

```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="289b", TAG+="uaccess"
```

then replug the adapter. Use `MODE="0666"` instead of `TAG+="uaccess"` on
systems without logind.

## Thirty-second tour

```c
#include "midas_feel/mf_raphnet.h"

/* once: send log lines into your port's console (default: stderr) */
mf_raphnet_set_log(my_log, NULL);

/* startup and hotplug: is an adapter answering? which ports have a pad? */
if (mf_raphnet_available())
    for (int ch = 0; ch < MF_RAPHNET_MAX_CHAN; ch++)
        if (mf_raphnet_peek(ch) == 1) assign_player_to_raphnet(ch);

/* every pad read, for a player on channel ch: real N64 values, no translation */
uint16_t btn; int8_t sx, sy;
if (mf_raphnet_poll(ch, &btn, &sx, &sy) == 1) {
    pad->button |= btn;   /* CONT_* layout */
    pad->stick_x = sx;    /* N64 units: no deadzone, no curve */
    pad->stick_y = sy;
}

/* where the port implements rumble (osMotorStart / osMotorStop) */
mf_raphnet_rumble(ch, on);
```

`mf_raphnet_poll` returns 1 when a pad answered, 0 when the port is empty, and
−1 for an adapter error or no adapter. Make every call from one thread, your
input thread.

## Status

The transport is the shipping Project Midas code, with logging and
configuration moved to setter functions. It builds with GCC (SDL2 2.30) and
MinGW with no warnings. This extracted copy has **not yet been run against a
real adapter**, so run `mf_raphnet_probe` before relying on it. The SDL3 path
has not been compiled.

## Licence and credits

MIT (see LICENSE), same as Project Midas.

- The RAW protocol is a clean-room implementation from the publicly documented
  wire format used by pj64raphnetraw and gcn64lib (raphnet technologies). No
  GPL code was copied.
- The Rumble Pak CRCs are the standard N64 pak algorithms.
