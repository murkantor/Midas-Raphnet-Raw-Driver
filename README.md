# Decomp / Recomp Raphnet RAW Driver

The raphnet N64-to-USB adapter driver from **Project Midas** (the GoldenEye 007
PC port), as a small standalone C library for other PC decomps and recomps.

Players praise how a real N64 controller feels in Project Midas. This is the
reason: the game gets **the controller's real poll response**, the same 4 bytes
an N64 console reads, with nothing in between.

- **No OS joystick layer.** It talks to the adapter's vendor HID interface with
  the raw SI get-status command. On Windows, the normal joystick path applies the
  OS's own per-device calibration and scaling, so its values are neither the real
  stick nor the same on two machines. This driver skips all of that.
- **Zero translation.** Buttons arrive in the N64 `CONT_*` bit layout, and the
  stick arrives as signed N64 units.
- **No frame-rate cost.** One USB exchange takes about 5 ms however tight the
  loop is. Done on the frame thread, it capped Project Midas at about 140 fps.
  Polling runs on a background thread that blocks in the kernel and never spins
  (so it works on one core too). The game thread reads a lock-free 32-bit cache.
- **Hot and idle channels.** The port being played is polled every pass. Empty
  ports drop to a 4 Hz rescan, so they don't steal bus time and drop short taps.
- **Real Rumble Pak**, through the actual pak protocol (arm block, motor block,
  address and data CRCs). It needs no firmware support. Rumble is desired state
  rather than a queue, so it never backs up or delays a stick read. With no pak
  inserted it costs nothing.
- **Self-healing.** A stale handle (USB re-enumeration, hub power, selective
  suspend) is reopened at most once a second, and a replugged adapter comes back
  without a restart.
- Single- and dual-port adapters, and firmware before and after v3.4.

## Add it to your game with one prompt

In Claude Code (or another AI coding agent), from your game's repository:

```
Add https://github.com/murkantor/Decomp-Recomp-Raphnet-Raw-Driver to this project. Follow its CLAUDE.md.
```

[CLAUDE.md](CLAUDE.md) tells the agent how to vendor the driver, find where your
port builds its pad state, keep the raphnet stick untouched, and hide the
adapter's duplicate joystick. It also lists what you need to test with a real
adapter.

## Build

```bash
cmake -S . -B build
cmake --build build
./build/mf_raphnet_probe          # live readout of both channels
./build/mf_raphnet_probe rumble   # also pulses the Rumble Pak on channel 0
```

| Host | Transport | Needs |
|---|---|---|
| Windows | native SetupDi + `HidD_*` (SDL's hidapi *filters out* the vendor interface on Windows, so SDL isn't used) | links `hid`, `setupapi` |
| Linux / macOS | SDL2 ≥ 2.0.18 `SDL_hid_*` | SDL2; on Linux, hidraw access (below) |
| SDL3 | `-DRAPHNET_RAW_SDL3=ON` | **not yet compiled or tested** |

As a subproject:

```cmake
add_subdirectory(third_party/raphnet-raw-driver)
target_link_libraries(game PRIVATE midas_feel_raphnet)
```

Without CMake: compile `src/mf_raphnet.c` and add `include/` to your include
path. The `midas_feel/` header path and `mf_` prefix match the copy kept in
Project Midas (`libs/midas-feel`), so changes can move between the two.

**Linux permissions.** Add `/etc/udev/rules.d/70-raphnet.rules`:

```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="289b", TAG+="uaccess"
```

then replug the adapter. Use `MODE="0666"` instead of `TAG+="uaccess"` on
systems without logind.

## Using it

```c
#include "midas_feel/mf_raphnet.h"

/* once: send log lines into your port's console (default: stderr) */
mf_raphnet_set_log(my_log, NULL);

/* startup and hotplug: is an adapter answering? which ports have a pad? */
if (mf_raphnet_available())
    for (int ch = 0; ch < MF_RAPHNET_MAX_CHAN; ch++)
        if (mf_raphnet_peek(ch) == 1) assign_player_to_raphnet(ch);

/* every pad read, for a player on channel ch */
uint16_t btn; int8_t sx, sy;
if (mf_raphnet_poll(ch, &btn, &sx, &sy) == 1) {
    pad->button |= btn;   /* CONT_* layout */
    pad->stick_x = sx;    /* N64 units */
    pad->stick_y = sy;
}

/* where the port implements rumble (osMotorStart / osMotorStop) */
mf_raphnet_rumble(ch, on);
```

`mf_raphnet_poll` returns 1 when a pad answered, 0 when the port is empty, and
−1 for an adapter error or no adapter. Make every call from one thread, your
input thread.

## Merging it into a port

### The one rule

**Give the game the bytes and nothing else.** Most of the feel is lost when a
port puts its own deadzone, response curve, square-to-circle mapping or octagon
clamp on top. The N64 game already has its own dead band and curve, tuned for
exactly this stick. For a raphnet player, skip every stick-processing step the
port applies to modern pads.

### Checklist

- [ ] **Find where the port builds `OSContPad`** (an `osContGetReadData`
      replacement, or the controller layer of a shared port framework). Raphnet
      goes in there.
- [ ] **Hide the joystick twin.** The adapter *also* enumerates as an ordinary
      joystick. Don't open that as a pad as well, or the player gets doubled
      input. Project Midas counts a device as raphnet-class if its SDL2 GUID
      string has `9b28` at character 8 (vendor `0x289B`, little-endian) or its
      name contains "raphnet". Such a device is routed to the RAW channel, even
      when a button is pressed while the vendor interface is momentarily down:
      the driver reconnects by itself, so pin the channel, not the joystick GUID.
- [ ] **Use `peek`, not `poll`, for presence checks.** `poll` marks a channel
      hot, and polling an empty channel takes bus time from the pad in play.
      Project Midas rechecks unassigned channels about once a second.
- [ ] **Fallback.** If the RAW path isn't available (permissions, a tool holding
      the adapter), read the adapter through the normal joystick API and divide
      each axis by 256. The adapter reports N64 units × 256 there, so you still
      get the true values with no deadzone. Clamp to ±127.
- [ ] **Rumble** maps one-to-one onto the game's motor start and stop calls.
- [ ] **Tools that share the adapter** with a running game (launchers, probes)
      call `mf_raphnet_set_probe_mode()` first, so their exit doesn't silence
      the game's rumble.

### N64 decomps and recomps

This is the natural fit: the bytes go straight into `OSContPad`.

- **Decomps:** hook the port's `OSContPad` producer.
- **Recomps:** the recompiled code reads pads through the runtime's input
  layer, so the hook goes there rather than in game code.

### Non-N64 games (GameCube, PS2, Xbox ports)

You can offer a real N64 pad in any port. The stick is in N64 units, so divide
by about 80 (a typical real pad's reach) and clamp to ±1 to get a unit value,
then convert to the target's range:

- **GameCube `PADStatus`:** an `s8` stick.
- **PS2 `libpad`:** `128 + v * 127`, Y down.
- **Xbox:** `SHORT`, Y up.

Don't put it through octagon or deadzone code: it's already a real stick with
its own centre.

GameCube controllers on a raphnet GC adapter aren't handled by this driver. It
sends the N64 get-status command only. A GC pad goes through the normal
joystick path, and its range there hasn't been verified.

## Status

- Windows transport: the shipping Project Midas code, with logging and
  configuration moved to setter functions. Builds with MinGW, no warnings.
- SDL2 transport: builds against SDL2 2.30, no warnings.
- This extracted copy has **not yet been run against a real adapter**. Run
  `mf_raphnet_probe` before relying on it.
- SDL3: not yet compiled.

See [docs/PROVENANCE.md](docs/PROVENANCE.md) for how this maps to the Project
Midas source.

## Licence and credits

MIT (see LICENSE), same as Project Midas. The RAW protocol is a clean-room
implementation from the publicly documented wire format used by pj64raphnetraw
and gcn64lib (raphnet technologies). No GPL code was copied. The Rumble Pak CRCs
are the standard N64 pak algorithms.

The mouse, controller-stick and look-curve code from Project Midas lives in a
separate repository.
