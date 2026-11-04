/**
 * mf_raphnet.c - raphnet GC/N64-to-USB adapter, RAW N64 backend.
 * See include/midas_feel/mf_raphnet.h. Source: Project Midas port/src/raphnet.c,
 * with the game hooks (portConLogf, GE_* environment switches) replaced by the
 * mf_raphnet_set_* calls.
 *
 * Clean-room implementation from the publicly documented wire format (the
 * pj64raphnetraw / gcn64lib protocol; the constants are interoperability
 * facts, no GPL code copied):
 *  - HID FEATURE reports, report id 0, report size 63 (firmware v3.4+; 40 older).
 *  - Request: [0x80 = raw SI command, channel, tx_len, payload...].
 *  - Reply (poll get-feature until the first byte echoes the request):
 *    [0x80, channel, rx_len, data...].
 *  - N64 get-status SI command 0x01 -> 4 bytes: buttons hi, buttons lo,
 *    stick X, stick Y. The button word IS the CONT_* layout and the stick
 *    bytes are signed N64 units.
 *
 * Windows: SDL's bundled hidapi FILTERS the vendor interface out of
 * SDL_hid_enumerate, so the Win32 HID API is used directly - SetupDi interface
 * enumeration + HidD_SetFeature/HidD_GetFeature. Other hosts use SDL_hid_*,
 * whose hidraw enumeration is unfiltered.
 */
#include "midas_feel/mf_raphnet.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int32_t  s32;

/* No SDL.h in the Windows branch: the Win32 transport needs nothing from SDL,
 * and some decomp trees ship freestanding libc headers that fight windows.h. */
#ifndef _WIN32
#ifdef MF_RAPHNET_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif
#endif

#define RQ_RAW_SI_COMMAND    0x80
#define RQ_SUSPEND_POLLING   0x03
#define N64_GET_STATUS       0x01
#define RAPHNET_MAX_CHAN     MF_RAPHNET_MAX_CHAN
#define RAPHNET_REPORT       63   /* v3.4+; pre-3.4 firmware falls back to 40 */

static int s_ReportSize = RAPHNET_REPORT;
static int s_Tried;

/* Transport health: the handle can go stale without anyone noticing (USB
 * re-enumeration, hub power event, selective suspend), after which every
 * feature-report call fails forever. After RN_FAIL_LIMIT consecutive failures
 * the handle is closed and re-opened, at most once per RN_RETRY_MS. */
#define RN_FAIL_LIMIT   20
#define RN_RETRY_MS   1000
static int s_Fails;
static int s_Lost;            /* 1 = reported the drop, waiting to recover */
static int s_Suspended;       /* adapter's own polling suspended (raw mode) */
static unsigned s_NextTry;
static unsigned s_OpenRetries;
static int s_Dbg;
static int s_RateLog;
static int s_WantThread = 1;

/* ---- logging ------------------------------------------------------------ */

static mf_raphnet_log_fn s_LogFn;
static void *s_LogUser;

void mf_raphnet_set_log(mf_raphnet_log_fn fn, void *user)
{
    s_LogFn = fn;
    s_LogUser = user;
}

void mf_raphnet_set_debug(int on)     { s_Dbg = (on != 0); }
void mf_raphnet_set_rate_log(int on)  { s_RateLog = (on != 0); }
void mf_raphnet_set_threaded(int on)  { s_WantThread = (on != 0); }

static void rnLogf(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (s_LogFn != NULL)
    {
        s_LogFn(line, s_LogUser);
    }
    else
    {
        fputs(line, stderr);
    }
}

/* ==== Windows: native HID feature-report transport ======================= */
#ifdef _WIN32

#include <windows.h>
#include <setupapi.h>

static void rnMemset(void *dst, int v, unsigned n)
{
    unsigned char *d = (unsigned char *) dst;
    while (n--) *d++ = (unsigned char) v;
}
static void rnMemcpy(void *dst, const void *src, unsigned n)
{
    unsigned char *d = (unsigned char *) dst;
    const unsigned char *sp = (const unsigned char *) src;
    while (n--) *d++ = *sp++;
}
static int rnContainsNoCase(const char *hay, const char *needle)
{
    int i, j;
    for (i = 0; hay[i] != '\0'; i++)
    {
        for (j = 0; needle[j] != '\0'; j++)
        {
            char h = hay[i + j];
            if (h >= 'A' && h <= 'Z') h = (char) (h - 'A' + 'a');
            if (h != needle[j]) break;
        }
        if (needle[j] == '\0') return 1;
    }
    return 0;
}
#define RN_MEMSET(d, v, n) rnMemset((d), (v), (unsigned) (n))
#define RN_MEMCPY(d, s, n) rnMemcpy((d), (s), (unsigned) (n))
#define RN_DELAY(ms) Sleep(ms)

/* hid.dll (link hid); declared locally so no DDK headers are needed */
typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } RN_HIDD_ATTRIBUTES;
__declspec(dllimport) void __stdcall HidD_GetHidGuid(GUID *guid);
__declspec(dllimport) BOOLEAN __stdcall HidD_SetFeature(HANDLE h, PVOID buf, ULONG len);
__declspec(dllimport) BOOLEAN __stdcall HidD_GetFeature(HANDLE h, PVOID buf, ULONG len);

static HANDLE s_Dev = INVALID_HANDLE_VALUE;

static int raphnetSendFeature(const u8 *buf, int len)
{
    return HidD_SetFeature(s_Dev, (PVOID) buf, (ULONG) len) ? 0 : -1;
}

static int raphnetGetFeature(u8 *buf, int len)
{
    return HidD_GetFeature(s_Dev, (PVOID) buf, (ULONG) len) ? len : -1;
}

static int raphnetExchangeOnce(const u8 *cmd, int cmdlen, u8 *rep, int repmax);

/* Open the raphnet vendor interface: probe EVERY interface whose path carries
 * vid_289b - it is mi_01 on single-port adapters but mi_02 on dual-port ones
 * (PID_0064 exposes MI_00/01/02). The raw-SI answer IS the test; joystick
 * interfaces simply do not reply. */
static int raphnetOpen(void)
{
    GUID hidGuid;
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD index;

    if (s_Tried)
    {
        return s_Dev != INVALID_HANDLE_VALUE;
    }
    s_Tried = 1;

    HidD_GetHidGuid(&hidGuid);
    devs = SetupDiGetClassDevsA(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    ifd.cbSize = sizeof(ifd);
    for (index = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &hidGuid, index, &ifd); index++)
    {
        char detailbuf[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 512];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *) detailbuf;
        const char *p;

        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devs, &ifd, detail, sizeof(detailbuf), NULL, NULL))
        {
            continue;
        }
        p = detail->DevicePath;
        if (!rnContainsNoCase(p, "vid_289b"))
        {
            continue;
        }

        s_Dev = CreateFileA(p, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
        if (s_Dev == INVALID_HANDLE_VALUE)
        {
            if (s_OpenRetries <= 1)
            {
                rnLogf("raphnet: open failed for %s\n", p);
            }
            continue;
        }

        {
            u8 cmd[4] = { RQ_RAW_SI_COMMAND, 0, 1, N64_GET_STATUS };
            u8 rep[8];

            s_ReportSize = RAPHNET_REPORT;
            if (raphnetExchangeOnce(cmd, 4, rep, sizeof(rep)) >= 3)
            {
                rnLogf("raphnet: RAW adapter online (true N64 poll path)\nraphnet: interface %s\n", p);
                break;
            }
            s_ReportSize = 40;   /* pre-3.4 firmware report size */
            if (raphnetExchangeOnce(cmd, 4, rep, sizeof(rep)) >= 3)
            {
                rnLogf("raphnet: RAW adapter online (pre-3.4 firmware)\nraphnet: interface %s\n", p);
                break;
            }
            if (s_OpenRetries <= 1)
            {
                rnLogf("raphnet: no protocol answer from %s\n", p);
            }
        }
        CloseHandle(s_Dev);
        s_Dev = INVALID_HANDLE_VALUE;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return s_Dev != INVALID_HANDLE_VALUE;
}

static int raphnetIsOpen(void)
{
    return s_Dev != INVALID_HANDLE_VALUE;
}

#define RN_TICKS() ((unsigned) GetTickCount())

static unsigned long long rnUsec(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    {   /* split, or count * 1e6 overflows after ~21 days of uptime */
        const unsigned long long q = (unsigned long long) now.QuadPart;
        const unsigned long long f = (unsigned long long) freq.QuadPart;
        return (q / f) * 1000000ull + (q % f) * 1000000ull / f;
    }
}
#define RN_USEC() rnUsec()

static void raphnetCloseDev(void)
{
    if (s_Dev != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s_Dev);
        s_Dev = INVALID_HANDLE_VALUE;
    }
}

/* ==== other hosts: SDL's hidapi ========================================== */
#elif defined(MF_RAPHNET_SDL3) || SDL_VERSION_ATLEAST(2, 0, 18)

#define RN_MEMSET SDL_memset
#define RN_MEMCPY SDL_memcpy
#define RN_DELAY(ms) SDL_Delay(ms)

static SDL_hid_device *s_Dev;

static int raphnetSendFeature(const u8 *buf, int len)
{
    return (SDL_hid_send_feature_report(s_Dev, buf, (size_t) len) < 0) ? -1 : 0;
}

static int raphnetGetFeature(u8 *buf, int len)
{
    return SDL_hid_get_feature_report(s_Dev, buf, (size_t) len);
}

static int raphnetExchangeOnce(const u8 *cmd, int cmdlen, u8 *rep, int repmax);

static int raphnetOpen(void)
{
    SDL_hid_device_info *list;
    SDL_hid_device_info *d;

    if (s_Tried)
    {
        return s_Dev != NULL;
    }
    s_Tried = 1;

    if (SDL_hid_init() != 0)
    {
        if (s_OpenRetries <= 1)
        {
            rnLogf("raphnet: SDL_hid_init failed: %s\n", SDL_GetError());
        }
        return 0;
    }
    list = SDL_hid_enumerate(MF_RAPHNET_VID, 0);
    for (d = list; d != NULL; d = d->next)
    {
        s_Dev = SDL_hid_open_path(d->path
#ifndef MF_RAPHNET_SDL3
                                  , 0
#endif
                                  );
        if (s_Dev == NULL)
        {
            if (s_OpenRetries <= 1)
            {
                rnLogf("raphnet: open failed for %s (%s) - check the hidraw permissions / udev rule for 289b\n",
                       d->path, SDL_GetError());
            }
            continue;
        }
        {
            u8 cmd[4] = { RQ_RAW_SI_COMMAND, 0, 1, N64_GET_STATUS };
            u8 rep[8];

            s_ReportSize = RAPHNET_REPORT;
            if (raphnetExchangeOnce(cmd, 4, rep, sizeof(rep)) >= 3)
            {
                rnLogf("raphnet: RAW adapter online (true N64 poll path)\n");
                break;
            }
            s_ReportSize = 40;
            if (raphnetExchangeOnce(cmd, 4, rep, sizeof(rep)) >= 3)
            {
                rnLogf("raphnet: RAW adapter online (pre-3.4 firmware)\n");
                break;
            }
            if (s_OpenRetries <= 1)
            {
                rnLogf("raphnet: no protocol answer from %s\n", d->path);
            }
        }
        SDL_hid_close(s_Dev);
        s_Dev = NULL;
    }
    SDL_hid_free_enumeration(list);
    return s_Dev != NULL;
}

static int raphnetIsOpen(void)
{
    return s_Dev != NULL;
}

#define RN_TICKS() ((unsigned) SDL_GetTicks())
static unsigned long long rnUsec(void)
{   /* split, or count * 1e6 overflows after ~21 days of uptime */
    const unsigned long long q = SDL_GetPerformanceCounter();
    const unsigned long long f = SDL_GetPerformanceFrequency();
    return (q / f) * 1000000ull + (q % f) * 1000000ull / f;
}
#define RN_USEC() rnUsec()

static void raphnetCloseDev(void)
{
    if (s_Dev != NULL)
    {
        SDL_hid_close(s_Dev);
        s_Dev = NULL;
    }
}

#else
#error "mf_raphnet.c needs Win32, SDL2 >= 2.0.18, or SDL3 (define MF_RAPHNET_SDL3)"
#endif

/* ==== shared protocol ==================================================== */

/* Transport timing for the optional rate line (device-owner thread only). */
static unsigned long long s_ExchUs;
static unsigned s_ExchN;
static unsigned s_ExchMaxUs;
static unsigned s_ConfirmN;
static unsigned s_RumbleWrites;
#define RN_NOPAK_RETRY_MS 3000
static unsigned s_NoPakUntil[RAPHNET_MAX_CHAN];
static u8 s_NoPakLogged[RAPHNET_MAX_CHAN];

static int raphnetExchangeOnceRaw(const u8 *cmd, int cmdlen, u8 *rep, int repmax);

static int raphnetExchangeOnce(const u8 *cmd, int cmdlen, u8 *rep, int repmax)
{
    const unsigned long long t0 = RN_USEC();
    const int n = raphnetExchangeOnceRaw(cmd, cmdlen, rep, repmax);
    const unsigned us = (unsigned) (RN_USEC() - t0);

    s_ExchUs += us;
    s_ExchN++;
    if (us > s_ExchMaxUs)
    {
        s_ExchMaxUs = us;
    }
    return n;
}

static int raphnetExchangeOnceRaw(const u8 *cmd, int cmdlen, u8 *rep, int repmax)
{
    u8 buf[RAPHNET_REPORT + 1];
    int tries;
    int n;

    RN_MEMSET(buf, 0, sizeof(buf));
    RN_MEMCPY(buf + 1, cmd, (cmdlen <= RAPHNET_REPORT) ? cmdlen : RAPHNET_REPORT);
    if (raphnetSendFeature(buf, s_ReportSize + 1) < 0)
    {
        return -1;
    }

    for (tries = 0; tries < 64; tries++)
    {
        RN_MEMSET(buf, 0, sizeof(buf));
        n = raphnetGetFeature(buf, s_ReportSize + 1);
        if (n < 0)
        {
            return -1;
        }
        if (buf[1] == cmd[0])
        {
            n = s_ReportSize;
            if (n > repmax)
            {
                n = repmax;
            }
            RN_MEMCPY(rep, buf + 1, n);
            return n;
        }
        RN_DELAY(1);
    }
    return -1;
}

/* Exchange with the self-healing transport (see the health block up top). */
static int raphnetExchange(const u8 *cmd, int cmdlen, u8 *rep, int repmax)
{
    int n = raphnetExchangeOnce(cmd, cmdlen, rep, repmax);

    if (n >= 0)
    {
        if (s_Lost)
        {
            rnLogf("raphnet: adapter recovered after %d failed exchanges\n", s_Fails);
            s_Lost = 0;
        }
        s_Fails = 0;
        return n;
    }

    s_Fails++;
    if (s_Dbg)
    {
        rnLogf("raphnet: exchange failed (cmd %02x, %d consecutive)\n", cmd[0], s_Fails);
    }
    if (s_Fails < RN_FAIL_LIMIT)
    {
        return -1;
    }

    /* Transport looks dead - drop the handle and try to re-acquire it. */
    {
        unsigned now = RN_TICKS();
        if (!s_Lost)
        {
            rnLogf("raphnet: adapter stopped responding - reconnecting\n");
            s_Lost = 1;
        }
        if (s_NextTry != 0 && (int) (now - s_NextTry) < 0)
        {
            return -1;   /* not yet time to retry */
        }
        s_NextTry = now + RN_RETRY_MS;

        raphnetCloseDev();
        s_Suspended = 0;    /* the reopened adapter starts unsuspended */
        s_Tried = 0;
        if (!raphnetOpen())
        {
            return -1;
        }
        rnLogf("raphnet: adapter re-opened\n");
        s_Fails = 0;
        n = raphnetExchangeOnce(cmd, cmdlen, rep, repmax);
        if (n >= 0 && s_Lost)
        {
            rnLogf("raphnet: adapter recovered\n");
            s_Lost = 0;
        }
    }
    return n;
}

/* raphnetOpen() with periodic retry while closed, so an unplugged pad comes
 * back on replug. Device-owner thread only. */
static int raphnetOpenRetry(void)
{
    if (s_Tried && !raphnetIsOpen())
    {
        const unsigned now = RN_TICKS();

        if (s_NextTry != 0 && (int) (now - s_NextTry) < 0)
        {
            return 0;
        }
        s_NextTry = now + RN_RETRY_MS;
        s_Suspended = 0;
        s_Tried = 0;
        s_OpenRetries++;
        if (!raphnetOpen())
        {
            return 0;
        }
        s_OpenRetries = 0;
        if (s_Lost)
        {
            rnLogf("raphnet: adapter re-opened\n");
        }
        return 1;
    }
    if (!raphnetOpen())
    {
        s_NextTry = RN_TICKS() + RN_RETRY_MS;   /* a failed first open starts the clock */
        return 0;
    }
    return 1;
}

/**
 * The adapter POLLS the controller itself for its joystick interface,
 * including pak management, which fights raw pak writes (every write is ACKed
 * but the motor stays still). Suspend that polling lazily on first raw use and
 * resume at exit so the adapter's normal joystick mode is not left dead.
 */
static void raphnetResumeAtExit(void)
{
    if (s_Suspended && raphnetIsOpen())
    {
        u8 cmd[2] = { RQ_SUSPEND_POLLING, 0 };
        u8 rep[8];
        raphnetExchange(cmd, 2, rep, sizeof(rep));
        s_Suspended = 0;
    }
}

static s32 s_ProbeMode;

void mf_raphnet_set_probe_mode(void)
{
    s_ProbeMode = 1;
}

/* Register the exit-time resume once, and never after the poller exists
 * (raphnetStopThread resumes after the join). */
static s32 s_ResumeArmed;

static void raphnetSuspendOnce(void)
{
    if (!s_ProbeMode && !s_Suspended && raphnetIsOpen())
    {
        u8 cmd[2] = { RQ_SUSPEND_POLLING, 1 };
        u8 rep[8];
        if (raphnetExchange(cmd, 2, rep, sizeof(rep)) >= 0)
        {
            s_Suspended = 1;
            if (!s_ResumeArmed)
            {
                s_ResumeArmed = 1;
                atexit(raphnetResumeAtExit);
            }
            rnLogf("raphnet: adapter polling suspended (raw session)\n");
        }
    }
}

/* ---- asynchronous transport -------------------------------------------
 *
 * An exchange is a synchronous USB control round trip scheduled on 1 ms bus
 * frames, so one poll costs several ms no matter how tight the retry loop is
 * (~5.3 ms measured). It cannot be fixed by polling less: the game's input
 * read is the producer of its sample stream. So the transport moves off the
 * frame thread.
 *
 * This is LATENCY HIDING, not parallelism - the thread is blocked in the
 * kernel - so it works on a single core too. Three properties keep that true:
 *   - The thread NEVER spins: it blocks in the USB call, and sleeps when idle.
 *   - No mutex: the whole sample is 4 bytes, packed into ONE aligned 32-bit
 *     word; a volatile store/load of it is atomic on every target. The
 *     button word is masked with 0xFF3F, leaving 0x40 and 0x80 for status.
 *   - Rumble is DESIRED STATE, not a queue, so it cannot back up.
 *
 * Exactly ONE thread ever touches the device. The caller's first poll happens
 * BEFORE the thread exists (it primes the cache synchronously, then starts it);
 * after that the caller only reads the cache.
 */
#define RN_CACHE_PRESENT 0x0040u
#define RN_CACHE_ERROR   0x0080u

static s32 raphnetPollHw(s32 chan, u16 *btn, s8 *x, s8 *y);
static s32 raphnetRumbleHw(s32 chan, s32 on);

#ifdef _WIN32
typedef HANDLE rn_thread_t;
#else
typedef SDL_Thread *rn_thread_t;
#endif

static volatile unsigned s_Polls;
static volatile u32 s_Cache[RAPHNET_MAX_CHAN];
static volatile u8  s_ChanActive[RAPHNET_MAX_CHAN];
static volatile s32 s_RumbleWant[RAPHNET_MAX_CHAN];
static volatile unsigned s_ChanAskMs[RAPHNET_MAX_CHAN];
static u8 s_HwPresent[RAPHNET_MAX_CHAN];
static volatile s32 s_ThreadStop;
static rn_thread_t  s_Thread;
static s32          s_ThreadMode = -1;   /* -1 unresolved, 0 inline, 1 threaded */

static u32 raphnetPack(s32 status, u16 btn, s8 x, s8 y)
{
    u32 w = (u32) (btn & 0xFF3Fu)
          | ((u32) (u8) x << 16)
          | ((u32) (u8) y << 24);

    if (status > 0)
    {
        w |= RN_CACHE_PRESENT;
    }
    else if (status < 0)
    {
        w |= RN_CACHE_ERROR;
    }
    return w;
}

/* A channel read within RN_HOT_MS is polled every pass; other active channels
 * every RN_IDLE_POLL_MS, so an empty port does not steal bus time from the pad
 * being played (short taps were being dropped). */
#define RN_HOT_MS        2000
#define RN_IDLE_POLL_MS   250
#define RN_RATE_LOG_MS  10000
#define RN_RUMBLE_MIN_MS  100

static void raphnetPollLoop(void)
{
    s32 rumbleHave[RAPHNET_MAX_CHAN];
    unsigned lastPoll[RAPHNET_MAX_CHAN];
    unsigned lastRumble[RAPHNET_MAX_CHAN];
    unsigned polls[RAPHNET_MAX_CHAN];
    unsigned rateMs = RN_TICKS();
    s32 chan;

    for (chan = 0; chan < RAPHNET_MAX_CHAN; chan++)
    {
        rumbleHave[chan] = -1;
        lastPoll[chan] = 0;
        lastRumble[chan] = rateMs - RN_RUMBLE_MIN_MS;
        polls[chan] = 0;
    }

    while (!s_ThreadStop)
    {
        int didwork = 0;
        unsigned now = RN_TICKS();

        for (chan = 0; chan < RAPHNET_MAX_CHAN; chan++)
        {
            u16 btn = 0;
            s8 x = 0;
            s8 y = 0;
            s32 r;

            if (!s_ChanActive[chan])
            {
                continue;   /* only channels the caller has asked for */
            }

            /* read first: hot channels every pass, idle ones on the slow rescan
             * (signed age: the caller can stamp an ask later than `now`) */
            if ((int) (now - s_ChanAskMs[chan]) < RN_HOT_MS
                || (unsigned) (now - lastPoll[chan]) >= RN_IDLE_POLL_MS)
            {
                lastPoll[chan] = now;
                r = raphnetPollHw(chan, &btn, &x, &y);
                s_Cache[chan] = raphnetPack(r, btn, x, y);
                s_Polls++;
                polls[chan]++;
                didwork = 1;
            }

            /* then rumble, after this pass's read, at most every
             * RN_RUMBLE_MIN_MS, so pulses never delay pad reads */
            if (s_RumbleWant[chan] != rumbleHave[chan]
                && (unsigned) (now - lastRumble[chan]) >= RN_RUMBLE_MIN_MS)
            {
                const s32 want = s_RumbleWant[chan];

                lastRumble[chan] = now;
                if (raphnetRumbleHw(chan, want) == 0)
                {
                    rumbleHave[chan] = want;
                }
                didwork = 1;
            }
        }

        if ((unsigned) (now - rateMs) >= RN_RATE_LOG_MS)
        {
            if (s_RateLog)
            {
                const unsigned dt = now - rateMs;
                const unsigned avg10 = s_ExchN ? (unsigned) (s_ExchUs * 10 / s_ExchN / 1000) : 0;
                const unsigned max10 = s_ExchMaxUs / 100;
                char line[256];
                int len;

                len = snprintf(line, sizeof(line), "raphnet: rate");
                for (chan = 0; chan < RAPHNET_MAX_CHAN; chan++)
                {
                    const u32 w = s_Cache[chan];

                    if (len >= 0 && len < (int) sizeof(line))
                    {
                        len += snprintf(line + len, sizeof(line) - (size_t) len, " | ch%d %uHz %s %s", (int) chan,
                                        (polls[chan] * 1000 + dt / 2) / dt,
                                        !s_ChanActive[chan] ? "off" : ((int) (now - s_ChanAskMs[chan]) < RN_HOT_MS) ? "hot" : "idle",
                                        (w & RN_CACHE_ERROR) ? "error" : (w & RN_CACHE_PRESENT) ? "pad" : "empty");
                    }
                }
                if (len >= 0 && len < (int) sizeof(line))
                {
                    snprintf(line + len, sizeof(line) - (size_t) len,
                             " | exchange avg %u.%ums max %u.%ums, %u/s, %u confirms, %u rumble writes\n",
                             avg10 / 10, avg10 % 10, max10 / 10, max10 % 10,
                             (s_ExchN * 1000 + dt / 2) / dt, s_ConfirmN, s_RumbleWrites);
                }
                rnLogf("%s", line);
            }
            for (chan = 0; chan < RAPHNET_MAX_CHAN; chan++)
            {
                polls[chan] = 0;
            }
            s_ExchUs = 0;
            s_ExchN = 0;
            s_ExchMaxUs = 0;
            s_ConfirmN = 0;
            s_RumbleWrites = 0;
            rateMs = now;
        }

        /* never spin: with no active channel or no adapter the calls return at once */
        RN_DELAY(didwork ? 1 : 20);
    }
}

#ifdef _WIN32
static DWORD WINAPI raphnetThreadEntry(LPVOID unused)
{
    (void) unused;
    raphnetPollLoop();
    return 0;
}
#else
static int SDLCALL raphnetThreadEntry(void *unused)
{
    (void) unused;
    raphnetPollLoop();
    return 0;
}
#endif

static void raphnetStopThread(void)
{
    if (s_Thread != NULL)
    {
        s_ThreadStop = 1;
#ifdef _WIN32
        /* bounded by one exchange; resume only once the device has one owner again */
        if (WaitForSingleObject(s_Thread, 2000) == WAIT_OBJECT_0)
        {
            raphnetResumeAtExit();
        }
        CloseHandle(s_Thread);
#else
        SDL_WaitThread(s_Thread, NULL);
        raphnetResumeAtExit();
#endif
        s_Thread = NULL;
    }
}

void mf_raphnet_shutdown(void)
{
    raphnetStopThread();
    raphnetResumeAtExit();
    if (s_ThreadMode == 1)
    {
        s_ThreadMode = 0;   /* any later call polls inline instead of reading a dead cache */
    }
}

/* 1 = the thread refreshes the cache, 0 = poll inline. */
static s32 raphnetAsyncReady(void)
{
    if (s_ThreadMode < 0)
    {
        if (!s_WantThread)
        {
            s_ThreadMode = 0;
        }
        else
        {
            /* set before the thread can run, so the poller never registers
             * the resume itself */
            const s32 resumeArmed = s_ResumeArmed;

            s_ResumeArmed = 1;
#ifdef _WIN32
            s_Thread = CreateThread(NULL, 0, raphnetThreadEntry, NULL, 0, NULL);
#else
            s_Thread = SDL_CreateThread(raphnetThreadEntry, "mf-raphnet", NULL);
#endif
            if (s_Thread == NULL)
            {
                rnLogf("raphnet: poller thread failed to start - polling inline\n");
                s_ThreadMode = 0;
                s_ResumeArmed = resumeArmed;
            }
            else
            {
                /* joined at exit: a detached thread mid-transfer during
                 * teardown is a mystery-crash source */
                atexit(raphnetStopThread);
                s_ThreadMode = 1;
            }
        }
    }
    return s_ThreadMode;
}

/* ---- public API --------------------------------------------------------- */

int mf_raphnet_available(void)
{
    if (s_Thread != NULL)
    {
        return raphnetIsOpen() || s_Lost;
    }
    return raphnetOpenRetry() || s_Lost;
}

int mf_raphnet_poll(int chan, uint16_t *btn, int8_t *x, int8_t *y)
{
    u32 w;

    if (chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }
    s_ChanAskMs[chan] = RN_TICKS();   /* keeps the channel hot in the poller */

    if (!s_ChanActive[chan])
    {
        /* first use of this channel: before the poller starts, do a real
         * exchange so a startup probe sees present/empty correctly */
        if (s_Thread == NULL && s_ThreadMode != 0)
        {
            u16 b = 0;
            s8 bx = 0;
            s8 by = 0;
            s32 r = raphnetPollHw(chan, &b, &bx, &by);

            s_Cache[chan] = raphnetPack(r, b, bx, by);
        }
        s_ChanActive[chan] = 1;
    }

    if (raphnetAsyncReady() != 1)
    {
        return raphnetPollHw(chan, btn, x, y);
    }

    w = s_Cache[chan];
    if (btn != NULL)
    {
        *btn = (u16) (w & 0xFF3Fu);
    }
    if (x != NULL)
    {
        *x = (s8) ((w >> 16) & 0xFFu);
    }
    if (y != NULL)
    {
        *y = (s8) ((w >> 24) & 0xFFu);
    }
    return (w & RN_CACHE_ERROR) ? -1 : (w & RN_CACHE_PRESENT) ? 1 : 0;
}

int mf_raphnet_peek(int chan)
{
    u32 w;

    if (chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }
    if (s_Thread == NULL || !s_ChanActive[chan])
    {
        return mf_raphnet_poll(chan, NULL, NULL, NULL);
    }
    w = s_Cache[chan];
    return (w & RN_CACHE_ERROR) ? -1 : (w & RN_CACHE_PRESENT) ? 1 : 0;
}

unsigned mf_raphnet_poll_count(void)
{
    return s_Polls;
}

static s32 raphnetPollHw(s32 chan, u16 *btn, s8 *x, s8 *y)
{
    u8 cmd[4] = { RQ_RAW_SI_COMMAND, 0, 1, N64_GET_STATUS };
    u8 rep[16];
    int n;

    if (!raphnetOpenRetry() || !raphnetIsOpen() || chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }
    raphnetSuspendOnce();
    cmd[1] = (u8) chan;
    n = raphnetExchange(cmd, 4, rep, sizeof(rep));
    if (n < 3)
    {
        return -1;
    }
    if (rep[2] != 4)   /* rx_len: 0 = nothing plugged into that port */
    {
        /* re-check before reporting a present pad as unplugged: a late Rumble
         * Pak write reply shares command byte 0x80 and can be mistaken for
         * this poll's reply. An already-empty port is believed without it. */
        if (!s_HwPresent[chan])
        {
            return 0;
        }
        s_ConfirmN++;
        n = raphnetExchange(cmd, 4, rep, sizeof(rep));
        if (n < 3)
        {
            return -1;
        }
        if (rep[2] != 4)
        {
            s_HwPresent[chan] = 0;
            return 0;
        }
    }
    s_HwPresent[chan] = 1;
    if (btn != NULL)
    {
        *btn = (u16) (((rep[3] << 8) | rep[4]) & 0xFF3F);   /* drop reset/unused bits */
    }
    if (x != NULL)
    {
        *x = (s8) rep[5];
    }
    if (y != NULL)
    {
        *y = (s8) rep[6];
    }
    return 1;
}

/**
 * Rumble Pak drive - the REAL pak protocol over raw SI, byte-identical to what
 * an N64 game's libultra osMotor* does: arm the pak once by writing 32 x 0x80 to
 * block 1024 (address 0x8000), then drive the motor with 32 x 0x01 / 32 x 0x00
 * blocks to block 1536 (0xC000), each a CONT_CMD_WRITE_MEMPACK (0x03) with the
 * 5-bit address CRC. (The adapter's managed SET_VIBRATION did nothing on v3.6
 * firmware in raw use; this path needs no firmware feature.)
 */
static u8 rnPakAddrCrc(u16 addr)
{
    u8 temp = 0;
    u8 temp2;
    int i;

    for (i = 0; i < 16; i++)
    {
        temp2 = (temp & 0x10) ? 21 : 0;
        temp <<= 1;
        temp |= (u8) ((addr & 0x400) ? 1 : 0);
        addr <<= 1;
        temp ^= temp2;
    }
    return temp & 0x1f;
}

static u8 rnPakDataCrc(u8 fill)
{
    u8 temp = 0;
    u8 temp2;
    int i;
    int j;

    for (i = 0; i <= 32; i++)
    {
        for (j = 7; j >= 0; j--)
        {
            temp2 = (temp & 0x80) ? 133 : 0;
            temp <<= 1;
            if (i < 32)
            {
                temp |= (u8) ((fill & (1 << j)) ? 1 : 0);
            }
            temp ^= temp2;
        }
    }
    return temp;
}

static s32 raphnetPakWrite(s32 chan, u16 block, u8 fill)
{
    u8 cmd[3 + 35];
    u8 rep[8];
    u16 af = (u16) ((block << 5) | rnPakAddrCrc(block));
    int i;
    int n;

    cmd[0] = RQ_RAW_SI_COMMAND;
    cmd[1] = (u8) chan;
    cmd[2] = 35;                 /* CONT_CMD_WRITE_MEMPACK_TX */
    cmd[3] = 0x03;               /* CONT_CMD_WRITE_MEMPACK */
    cmd[4] = (u8) (af >> 8);
    cmd[5] = (u8) (af & 0xFF);
    for (i = 0; i < 32; i++)
    {
        cmd[6 + i] = fill;
    }
    n = raphnetExchange(cmd, sizeof(cmd), rep, sizeof(rep));
    if (n < 4)
    {
        return -1;   /* rx_len 1 needs four bytes: header, channel, length, CRC */
    }
    /* the controller answers with the data CRC whether or not a pak is in;
     * only that CRC inverted means an empty slot */
    if (rep[2] == 1 && rep[3] == (u8) ~rnPakDataCrc(fill))
    {
        return -1;
    }
    return (rep[2] == 1) ? 0 : -1;
}

int mf_raphnet_rumble(int chan, int on)
{
    if (chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }
    if (raphnetAsyncReady() != 1)
    {
        return raphnetRumbleHw(chan, on);
    }
    s_RumbleWant[chan] = (on != 0);
    return 0;
}

static s32 raphnetRumbleHw(s32 chan, s32 on)
{
    static s8 last[RAPHNET_MAX_CHAN] = { -1, -1 };
    static u8 armed[RAPHNET_MAX_CHAN];
    /* a failed motor write may still have landed, so until a write succeeds an
     * OFF still goes out; after a few failures the edge counts as handled */
    static u8 unsure[RAPHNET_MAX_CHAN];
    static u8 fails[RAPHNET_MAX_CHAN];

    if (!raphnetIsOpen() || chan < 0 || chan >= RAPHNET_MAX_CHAN)
    {
        return -1;
    }
    on = (on != 0);
    if (last[chan] == (s8) on)
    {
        return 0;
    }
    /* no rumble pak must cost nothing: stopping a never-armed motor is a
     * no-op, and a pak that did not answer the arm is left alone for
     * RN_NOPAK_RETRY_MS (retrying ahead of every read caused input lag) */
    if (!armed[chan] && !on && !unsure[chan])
    {
        last[chan] = 0;
        return 0;
    }
    if (!armed[chan] && !(unsure[chan] && !on)
        && s_NoPakUntil[chan] != 0 && (int) (RN_TICKS() - s_NoPakUntil[chan]) < 0)
    {
        return 0;
    }
    raphnetSuspendOnce();

    if (!armed[chan])
    {
        s_RumbleWrites++;
        if (raphnetPakWrite(chan, 1024, 0x80) != 0)   /* osMotorInit arm */
        {
            if (!s_NoPakLogged[chan])
            {
                s_NoPakLogged[chan] = 1;
                if (s_RateLog || s_Dbg)
                {
                    rnLogf("raphnet: no Rumble Pak answered on ch%d - retried every %ds, not every poll\n",
                           (int) chan, RN_NOPAK_RETRY_MS / 1000);
                }
            }
            s_NoPakUntil[chan] = RN_TICKS() + RN_NOPAK_RETRY_MS;
            return 0;
        }
        armed[chan] = 1;
    }
    s_RumbleWrites++;
    if (raphnetPakWrite(chan, 1536, on ? 0x01 : 0x00) != 0)
    {
        armed[chan] = 0;      /* pak vanished mid-session: re-arm next time */
        s_NoPakUntil[chan] = RN_TICKS() + RN_NOPAK_RETRY_MS;
        last[chan] = -1;      /* motor state unknown */
        unsure[chan] = 1;
        if (++fails[chan] < 3)
        {
            return -1;        /* the poller keeps have behind want: retried */
        }
        fails[chan] = 0;
        return 0;
    }
    last[chan] = (s8) on;
    unsure[chan] = 0;
    fails[chan] = 0;
    return 0;
}
