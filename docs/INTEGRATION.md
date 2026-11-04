# Merging the raphnet RAW driver into another port

This covers PC decomps and recomps of N64 games, and offering an N64 pad in
GameCube, PS2 and Xbox ports. Every port's input layer is different, so this
tells you **what to find** in your port and **what to put there**.

## The one rule

**Give the game the bytes and nothing else.** Most of the feel is lost when a
port puts its own deadzone, response curve, square-to-circle mapping or octagon
clamp on top. The N64 game already has its own dead band and curve, tuned for
exactly this stick. For a raphnet player, skip every stick-processing step the
port applies to modern pads.

## Checklist

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

## N64 decomps and recomps

This is the natural fit: the bytes go straight into `OSContPad`.

- **Decomps:** hook the port's `OSContPad` producer.
- **Recomps:** the recompiled code reads pads through the runtime's input
  layer, so the hook goes there rather than in game code.

## Non-N64 games (GameCube, PS2, Xbox ports)

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
