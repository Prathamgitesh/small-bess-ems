/* dashboard.c — reads FIFO, prints live table to terminal
 *
 * SYSCALLS / CONCEPTS USED HERE:
 *   open()           open named FIFO for reading
 *   read()           read JSON lines from FIFO
 *   sscanf()         parse JSON fields (no full JSON parser needed)
 *   write() / printf print formatted table to stdout
 *   sigaction()      clean exit
 *
 * No ncurses — just plain printf, easy to read and debug.
 * One line printed per telemetry update (every 1 real second).
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include "../include/ipc_config.h"

static volatile sig_atomic_t g_running = 1;
static void on_term(int s) { (void)s; g_running = 0; }

/* mode code → readable string */
static const char *mode_str(int m) {
    switch (m) {
        case 0: return "IDLE     ";
        case 1: return "CHARGE   ";
        case 2: return "DISCHARGE";
        case 3: return "FAULT    ";
        default:return "UNKNOWN  ";
    }
}

int main(void) {
    struct sigaction sa = { .sa_handler = on_term };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    /* open FIFO read end — this unblocks simulator's open(O_WRONLY) */
    int fifo = open(FIFO_PATH, O_RDONLY);
    if (fifo < 0) { perror("open FIFO"); exit(1); }

    /* print table header */
    printf("\n%-12s %-6s %-8s %-8s %-5s\n",
           "MODE", "SoC%", "Volt(V)", "Curr(A)", "Fault");
    printf("%-12s %-6s %-8s %-8s %-5s\n",
           "------------", "------", "--------", "--------", "-----");

    char line[256];
    while (g_running) {
        /* read one JSON line from FIFO */
        ssize_t n = read(fifo, line, sizeof(line)-1);
        if (n <= 0) break;                      /* FIFO closed = simulator exited */
        line[n] = '\0';

        /* parse JSON fields with sscanf */
        double soc, voltage, current;
        int mode, fault;
        sscanf(line,
            "{\"soc\":%lf,\"v\":%lf,\"i\":%lf,\"mode\":%d,\"fault\":%d}",
            &soc, &voltage, &current, &mode, &fault);

        /* print one row */
        printf("%-12s %-6.1f %-8.2f %-8.1f %-5d\n",
               mode_str(mode), soc*100.0, voltage, current, fault);
        fflush(stdout);
    }

    close(fifo);
    return 0;
}
