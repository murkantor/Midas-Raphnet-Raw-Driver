/**
 * mf_raphnet.h - raphnet N64-to-USB adapter, RAW mode, lifted from Project Midas.
 *
 * Reads the adapter's vendor HID interface, which returns the ACTUAL N64
 * controller poll response - the same 4 bytes a console sees - instead of
 * going through the OS joystick stack (which on Windows applies its own
 * per-device calibration and scaling, different on every machine).
 *
 *   buttons  N64 CONT_* bit layout: A 0x8000, B 0x4000, Z 0x2000,
 *            START 0x1000, D-pad U/D/L/R 0x0800/0x0400/0x0200/0x0100,
 *            L 0x0020, R 0x0010, C U/D/L/R 0x0008/0x0004/0x0002/0x0001.
 *   stick    signed N64 units, Y up-positive. Zero translation needed.
 *
 * Also drives a real Rumble Pak with the real pak protocol (arm + motor
 * blocks, address and data CRCs), so rumble works without firmware help.
 *
 * Polling runs on a background thread that blocks in the USB call (it never
 * spins, so it also works on one core); the game thread reads a lock-free
 * 32-bit cache and never blocks. One USB exchange costs ~5 ms; doing it on
 * the frame thread capped Project Midas at ~140 fps.
 *
 * Hosts:
 *   Windows  native SetupDi + HidD_* (link hid, setupapi). SDL's bundled
 *            hidapi FILTERS the vendor interface out on Windows, so SDL is
 *            not used there at all.
 *   others   SDL2 >= 2.0.18 SDL_hid_* (define MF_RAPHNET_SDL3 for SDL3).
 *            Linux needs read/write access to the hidraw node: a udev rule
 *            for vendor 289b (see README.md).
 *
 * N64 controllers only. GameCube pads on a raphnet adapter go through the
 * normal joystick path (see README.md).
 *
 * Thread model: call everything below from ONE thread (your input thread).
 */
#ifndef MIDAS_FEEL_MF_RAPHNET_H
#define MIDAS_FEEL_MF_RAPHNET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MF_RAPHNET_VID       0x289B
#define MF_RAPHNET_MAX_CHAN  2       /* channels per adapter (dual-port models) */

/* N64 button bits, as returned in *buttons (same as libultra's CONT_*). */
#define MF_N64_A       0x8000
#define MF_N64_B       0x4000
#define MF_N64_Z       0x2000
#define MF_N64_START   0x1000
#define MF_N64_DUP     0x0800
#define MF_N64_DDOWN   0x0400
#define MF_N64_DLEFT   0x0200
#define MF_N64_DRIGHT  0x0100
#define MF_N64_L       0x0020
#define MF_N64_R       0x0010
#define MF_N64_CUP     0x0008
#define MF_N64_CDOWN   0x0004
#define MF_N64_CLEFT   0x0002
#define MF_N64_CRIGHT  0x0001

/**
 * Log sink. Called from your input thread, except the optional periodic rate
 * line (mf_raphnet_set_rate_log), which comes from the poller thread - make
 * the sink thread-safe if you enable that. Default: stderr.
 */
typedef void (*mf_raphnet_log_fn)(const char *line, void *user);
void mf_raphnet_set_log(mf_raphnet_log_fn fn, void *user);

/** 1 = trace every failed exchange (Project Midas: GE_RAPHDBG=1). */
void mf_raphnet_set_debug(int on);

/** 1 = log refresh rate and USB cost every 10 s from the poller thread. */
void mf_raphnet_set_rate_log(int on);

/**
 * 0 = poll inline on the calling thread (no poller thread) - for A/B tests
 * and tools. Must be called before the first poll. Default 1.
 */
void mf_raphnet_set_threaded(int on);

/**
 * Probe / tool processes that share the adapter with a running game must
 * never suspend the adapter's own polling (their exit-time resume would
 * silence the game's rumble). Call before the first poll.
 */
void mf_raphnet_set_probe_mode(void);

/**
 * Is the raw path usable (or reconnecting)? Opens the adapter on first call
 * and retries at most once a second while it is absent, so hotplug works.
 */
int mf_raphnet_available(void);

/**
 * Read one channel: 1 = a controller answered (buttons/x/y written),
 * 0 = nothing plugged into that port, -1 = adapter error / absent.
 * Threaded: a cache read, never blocks. Reading a channel keeps it "hot"
 * (polled every pass); unread channels drop to a 4 Hz presence rescan so an
 * empty port does not steal bus time from the pad being played.
 * Any out pointer may be NULL.
 */
int mf_raphnet_poll(int chan, uint16_t *buttons, int8_t *x, int8_t *y);

/** Like mf_raphnet_poll but does not mark the channel hot - for presence checks. */
int mf_raphnet_peek(int chan);

/**
 * Rumble Pak motor on/off. Threaded: records the DESIRED state and returns;
 * the poller applies it after its next read, at most every 100 ms per
 * channel, so rumble never delays pad reads. No pak = no cost.
 */
int mf_raphnet_rumble(int chan, int on);

/** Number of cache refreshes so far (to show the real poll rate in a HUD). */
unsigned mf_raphnet_poll_count(void);

/** Stop the poller and resume the adapter's own polling. Also run at exit. */
void mf_raphnet_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
