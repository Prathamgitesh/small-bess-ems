/*
 * dashboard.c
 * ===========
 * Reads JSON telemetry from the FIFO and prints a live table to the terminal.
 *
 * This is the simplest process — no threads, no shared memory, no mutex.
 * Just: open FIFO → read a line → parse it → print it → repeat.
 *
 * KEY LINUX CONCEPTS HERE:
 *   open()      - open the named FIFO for reading
 *                 (this unblocks simulator which is waiting for the read end)
 *   read()      - read one JSON line from FIFO
 *   sscanf()    - parse the JSON fields
 *   printf()    - print formatted output to terminal
 *   close()     - close FIFO on exit
 *   sigaction() - clean exit
 *
 * FIFO flow:
 *   simulator            FIFO                 dashboard
 *   write(JSON line) --> /tmp/bess_telem --> read(line)
 *
 * The FIFO blocks both sides until both are open:
 *   - simulator calls open(O_WRONLY) and waits
 *   - dashboard calls open(O_RDONLY) → both unblock and can communicate
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include "../include/ipc_config.h"

static volatile sig_atomic_t g_running = 1;
static void on_term(int s) { (void)s; g_running = 0; }

/* Convert mode integer to a readable string */
static const char *mode_name(int mode)
{
    switch (mode) {
        case 0: return "IDLE      ";
        case 1: return "CHARGING  ";
        case 2: return "DISCHARGE ";
        case 3: return "*** FAULT ";
        default:return "UNKNOWN   ";
    }
}

int main(void)
{
    fprintf(stderr, "[dash] dashboard starting (PID=%d)\n", getpid());

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    /*
     * Open the FIFO for reading.
     * This call BLOCKS until simulator opens it for writing.
     * That synchronisation point is intentional — we know simulator
     * is ready before we start printing anything.
     */
    int fifo = open(FIFO_PATH, O_RDONLY);
    if (fifo < 0) {
        perror("open FIFO");
        exit(1);
    }

    /* Print table header */
    printf("\n");
    printf("=================================================\n");
    printf("  BATTERY EMS — LIVE STATUS\n");
    printf("=================================================\n");
    printf("  %-12s  %-8s  %-8s  %-9s  %s\n",
           "MODE", "SoC %", "Volt V", "Current A", "Fault");
    printf("  %-12s  %-8s  %-8s  %-9s  %s\n",
           "------------", "-------", "-------", "---------", "-----");

    char line[256];  /* buffer to hold one JSON line */

    while (g_running) {

        /*
         * read() reads up to sizeof(line)-1 bytes.
         * Each JSON line from simulator ends with '\n'.
         * We read until we get data or the pipe is closed.
         */
        ssize_t n = read(fifo, line, sizeof(line) - 1);

        if (n == 0) {
            /* FIFO closed — simulator exited */
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read FIFO");
            break;
        }

        line[n] = '\0';  /* null-terminate so sscanf works */

        /* Parse the JSON fields using sscanf.
         * JSON: {"soc":0.603,"v":207.32,"i":50.0,"mode":1,"fault":0}
         *
         * We extract each value by matching the field name pattern.
         */
        double soc, voltage, current;
        int    mode, fault;

        int got = sscanf(line,
            "{\"soc\":%lf,\"v\":%lf,\"i\":%lf,\"mode\":%d,\"fault\":%d}",
            &soc, &voltage, &current, &mode, &fault);

        if (got != 5) continue;  /* skip malformed lines */

        /* Print one row of the live table */
        printf("  %-12s  %-7.1f  %-7.2f  %-9.1f  %d\n",
               mode_name(mode),
               soc * 100.0,   /* convert 0.0-1.0 to 0-100% */
               voltage,
               current,
               fault);
        fflush(stdout);  /* force output to appear immediately */
    }

    close(fifo);
    printf("\n[dashboard] simulation ended\n");
    return 0;
}
