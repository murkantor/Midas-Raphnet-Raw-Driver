/**
 * mf_raphnet_probe - is the raphnet RAW path working on this machine?
 *
 *   mf_raphnet_probe            print both channels ~10x a second for 10 s
 *   mf_raphnet_probe rumble     also pulse the Rumble Pak on channel 0
 *
 * Shows the real N64 values: buttons in CONT_* hex, stick in N64 units.
 * A healthy original pad reads about +/-80..85 on a cardinal.
 */
#include "midas_feel/mf_raphnet.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#ifdef MF_RAPHNET_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif
#define SLEEP_MS(ms) SDL_Delay(ms)
#endif

int main(int argc, char **argv)
{
    const int rumble = (argc > 1 && strcmp(argv[1], "rumble") == 0);
    int i, c;

    mf_raphnet_set_debug(1);
    mf_raphnet_set_rate_log(1);

    if (!mf_raphnet_available())
    {
        printf("no raphnet adapter answered the RAW protocol\n"
               "  Windows: is it plugged in, and not held open exclusively by another tool?\n"
               "  Linux:   needs read/write on /dev/hidraw* for vendor 289b (udev rule)\n");
        return 1;
    }

    for (i = 0; i < 100; i++)
    {
        printf("\r");
        for (c = 0; c < MF_RAPHNET_MAX_CHAN; c++)
        {
            uint16_t b = 0;
            int8_t x = 0, y = 0;
            const int r = mf_raphnet_poll(c, &b, &x, &y);

            if (r == 1)
            {
                printf("ch%d btn %04x stick %4d %4d   ", c, b, x, y);
            }
            else
            {
                printf("ch%d %-22s   ", c, (r == 0) ? "(no controller)" : "(adapter error)");
            }
        }
        fflush(stdout);
        if (rumble)
        {
            mf_raphnet_rumble(0, (i / 10) & 1);
        }
        SLEEP_MS(100);
    }
    printf("\n%u cache refreshes in ~10 s\n", mf_raphnet_poll_count());
    mf_raphnet_rumble(0, 0);
    SLEEP_MS(200);
    mf_raphnet_shutdown();
    return 0;
}
