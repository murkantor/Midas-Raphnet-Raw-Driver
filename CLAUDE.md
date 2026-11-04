# CLAUDE.md: adding the raphnet RAW driver to a game

You are probably reading this because someone asked you to add this driver to
*their* game (a PC decomp, recomp or port). This file is your playbook. Follow it
in order.

If you are instead working on the driver itself, skip to "Working on this
repository" at the end.

## The one rule

**A raphnet player's stick and buttons reach the game untouched.** No
deadzone, no response curve, no square-to-circle or octagon clamp, no
remapping of the stick. The driver already returns the real N64 poll response:
buttons in the `CONT_*` layout and the stick in signed N64 units. The game's own
code then does what it did on the console, and that's the whole point. If the
host's input code processes every pad's stick, add a raphnet branch that skips
that processing.

## Ground rules

- **Don't edit `src/mf_raphnet.c` to fit the host.** Adapt at the call site. If
  something in the driver really seems wrong, tell the user. The driver is kept
  in sync with Project Midas's copy.
- **Don't guess the host's architecture.** Find the real code (step 2) and cite
  `file:line` when you explain your plan.
- Call the driver from **one thread only**: the host's input thread.
- **Ask the user** about the choices that are theirs: which player slot a
  raphnet pad gets by default, whether to show it in a device menu, and whether
  to support it in non-N64 games at all.

## Step 1: vendor the driver

Pick whichever matches how the host already keeps third-party code:

```bash
git submodule add https://github.com/murkantor/Decomp-Recomp-Raphnet-Raw-Driver third_party/raphnet-raw-driver
```

or a `git subtree add`, or copy `include/`, `src/` and `LICENSE`. Then wire up
the build:

- **CMake:** `add_subdirectory(third_party/raphnet-raw-driver)` and
  `target_link_libraries(<game target> PRIVATE midas_feel_raphnet)`. An
  existing `SDL2::SDL2` target in the host is reused.
- **Other build systems:** compile `src/mf_raphnet.c` and add `include/`.
  - On Windows, link `hid` and `setupapi`. **Don't** include SDL in that file
    on Windows: the driver deliberately uses the Win32 HID API, because SDL's
    hidapi hides the vendor interface there.
  - Elsewhere, link SDL2 ≥ 2.0.18. For SDL3, define `MF_RAPHNET_SDL3`; that path
    is untested, so tell the user if the host uses SDL3.

## Step 2: find the hook points

Search the host and write these down with `file:line` before editing:

| Find | Search hints |
|---|---|
| where the port **builds the console pad struct** for each player | `OSContPad`, `osContGetReadData`, `osContStartReadData`, `PADStatus`, `padman`, `XINPUT_GAMEPAD` |
| where pads are **opened and assigned to player slots**, and hotplug | `SDL_CONTROLLERDEVICEADDED`, `SDL_JoystickOpen`, `SDL_GameControllerOpen`, `SDL_OpenGamepad` |
| where the host applies **deadzones, curves or stick clamps** | `deadzone`, `dead_zone`, `sensitivity`, `curve`, `clamp` |
| where **rumble** is implemented | `osMotorStart`, `osMotorStop`, `__osMotorAccess`, `SDL_GameControllerRumble` |
| the host's **log** function | for `mf_raphnet_set_log` |

## Step 3: wire it up

1. At input init, call `mf_raphnet_set_log(<host log adapter>, NULL)`.
2. **Assignment.** At startup and on every hotplug, if `mf_raphnet_available()`
   returns true, give each channel where `mf_raphnet_peek(ch) == 1` a player
   slot. Recheck unassigned channels about once a second with **`peek`**, never
   `poll`: polling an empty channel takes USB time from the pad being played.
3. **Hide the joystick twin.** The adapter also appears as an ordinary
   joystick. Don't open that as a pad too, or input is doubled. Treat a device
   as raphnet-class if its SDL2 GUID string has `9b28` at character 8 (VID
   `0x289B`, little-endian), or its name contains "raphnet" (case-insensitive).
   Route it to the RAW channel, and pin the channel rather than the joystick
   GUID, because the driver reconnects by itself after a drop.
4. **Every pad read** for a raphnet player on channel `ch`:
   ```c
   uint16_t btn; int8_t sx, sy;
   if (mf_raphnet_poll(ch, &btn, &sx, &sy) == 1) {
       pad->button |= btn;
       pad->stick_x = sx;
       pad->stick_y = sy;
   }
   ```
   and skip the host's stick processing for this player (the one rule).
5. **Rumble:** where the host starts or stops the motor for that player, call
   `mf_raphnet_rumble(ch, on)`.
6. **Fallback:** if the RAW path isn't available, read the adapter through the
   joystick API and divide each axis by 256, clamping to ±127. That still gives
   true N64 units. Don't apply a deadzone.
7. Any launcher or probe tool that shares the adapter with a running game
   calls `mf_raphnet_set_probe_mode()` before its first poll.
8. **Non-N64 hosts** (GameCube, PS2, Xbox): divide the stick by about 80, clamp
   to ±1, convert to the host's range, and still skip the deadzone and octagon
   code. Confirm with the user that they want this.

## Step 4: verify

1. Build the host for each platform it supports (at least Windows and Linux),
   and fix any warnings you introduced.
2. Build this repository's probe tool:
   `cmake -S third_party/raphnet-raw-driver -B build-rn && cmake --build build-rn`.
3. You almost certainly have no adapter. Say so plainly, and list what the user
   must check by hand:
   - `build-rn/mf_raphnet_probe` shows live values (a good pad reads about
     ±80–85 on a cardinal)
   - in game: buttons work, nothing is doubled, and the stick has no extra
     deadzone
   - `mf_raphnet_probe rumble` with a Rumble Pak, and rumble in game
   - unplug and replug mid-game: the pad comes back
   - on Linux, the udev rule from the README is in place
4. Don't claim the integration works on hardware unless the user has confirmed
   it.

## Step 5: hand back

Report the hook points (`file:line`), how assignment and the joystick twin are
handled, which host stick processing is bypassed for raphnet players, and the
manual test list above.

## Working on this repository

- C99, single source file, structurally in step with Project Midas
  `port/src/raphnet.c`; see `docs/PROVENANCE.md`. Carry transport fixes both
  ways.
- Keep the thread model: one device-owner thread, never spinning, a lock-free
  32-bit cache, and rumble as desired state.
- Build checks: `cmake -S . -B build && cmake --build build` (SDL2) and a MinGW
  cross-build (`x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -Iinclude
  src/mf_raphnet.c tools/mf_raphnet_probe.c -lhid -lsetupapi`). Both must stay
  warning-free.
