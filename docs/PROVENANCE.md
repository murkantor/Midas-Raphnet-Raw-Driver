# Where this came from

`src/mf_raphnet.c` is Project Midas `port/src/raphnet.c` (`portRaphnet*`), also
kept in Project Midas as `libs/midas-feel/src/mf_raphnet.c`. The structure is
unchanged on purpose, so a diff between the copies stays readable. Fix the
transport in one place, then carry the fix to the others.

| This driver | Project Midas |
|---|---|
| `mf_raphnet_available` | `portRaphnetAvailable` |
| `mf_raphnet_poll` | `portRaphnetPoll` |
| `mf_raphnet_peek` | `portRaphnetPeek` |
| `mf_raphnet_rumble` | `portRaphnetRumble` |
| `mf_raphnet_set_probe_mode` | `portRaphnetProbeMode` |
| `mf_raphnet_poll_count` | `g_PortRaphnetPolls` |
| `mf_raphnet_set_debug` | env `GE_RAPHDBG=1` |
| `mf_raphnet_set_threaded(0)` | env `GE_RAPHNET_SYNC=1` |
| `mf_raphnet_set_log` | `portConLogf` |
| `mf_raphnet_set_rate_log`, `mf_raphnet_shutdown` | new |

Deliberate differences:

- Logging goes through a callback. Lines start with `raphnet:` instead of `ge:`.
- The periodic rate line and the "no Rumble Pak" note are opt-in
  (`mf_raphnet_set_rate_log`); Project Midas always printed them.
- The Windows interface match for `vid_289b` is case-insensitive.
- SDL3 is an option for the non-Windows transport (`MF_RAPHNET_SDL3`). It is
  untested.
- `mf_raphnet_shutdown()` was added for ports that tear input down before exit.

Project Midas background reading: `docs/port/port-inputs-vs-n64-inputs.md`
("A raphnet is always the RAW path", "A raphnet is always N64") and
`docs/port/input-and-display.md` (device assignment, a raphnet on an `auto`
slot).
