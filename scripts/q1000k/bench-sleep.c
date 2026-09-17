/* SPDX-License-Identifier: GPL-2.0-only */
/* RAM-only delay shim for bench images whose BusyBox lacks fractional sleep.
 * All integer delays retain the image's existing /bin/sleep implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    struct timespec remaining;
    if (argc == 2 && (!strcmp(argv[1], "0.2") || !strcmp(argv[1], "0.25"))) {
        remaining.tv_sec = 0;
        remaining.tv_nsec = !strcmp(argv[1], "0.2") ? 200000000L : 250000000L;
        while (nanosleep(&remaining, &remaining))
            if (errno != EINTR) { perror("nanosleep"); return 1; }
        return 0;
    }
    execv("/bin/sleep", argv);
    perror("/bin/sleep");
    return 1;
}
