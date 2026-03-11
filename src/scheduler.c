/*
 * scheduler.c
 * ===========
 * Reads the load profile file line by line and sends commands to the
 * simulator via the message queue.
 *
 * Think of it as a pre-programmed timer:
 *   "At 0 seconds:  start charging at 50A"
 *   "After 30 secs: switch to discharging at 30A"
 *   "After 30 secs: discharge at 110A"  ← protection will trip this
 *   ...
 *
 * KEY LINUX CONCEPTS HERE:
 *   mq_open()   - open the message queue (created by supervisor)
 *   mq_send()   - put a command message into the queue
 *   fopen()     - open load_profile.txt for reading
 *   fgets()     - read one line at a time
 *   sscanf()    - parse a line into variables
 *   sleep()     - wait N seconds between steps
 *   sigaction() - clean exit on SIGTERM
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <mqueue.h>
#include "../include/ipc_config.h"
#include "../include/bess_types.h"

static volatile sig_atomic_t g_running = 1;
static void on_term(int s) { (void)s; g_running = 0; }

int main(void)
{
    fprintf(stderr, "[sched] scheduler starting (PID=%d)\n", getpid());

    /* Install signal handler for clean shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    /* Open the message queue for writing (O_WRONLY = write only) */
    mqd_t mq = mq_open(MQ_NAME, O_WRONLY);
    if (mq == (mqd_t)-1) {
        perror("mq_open");
        exit(1);
    }

    /* Open the load profile file */
    FILE *fp = fopen("data/load_profile.txt", "r");
    if (!fp) {
        perror("fopen load_profile.txt");
        exit(1);
    }

    /*
     * Read the file line by line.
     *
     * File format (see data/load_profile.txt):
     *   duration_in_seconds  COMMAND  amps  description
     *
     * Example line:
     *   30  CHARGE  50  Off-peak charge
     *
     * Lines starting with '#' are comments (we skip them).
     */
    char line[128];

    while (g_running && fgets(line, sizeof(line), fp)) {

        /* Skip comment lines and blank lines */
        if (line[0] == '#' || line[0] == '\n') continue;

        /* Parse the line into 4 fields */
        int    duration;
        char   command_type[16];
        double amps;
        char   description[48];

        int parsed = sscanf(line, "%d %15s %lf %47[^\n]",
                            &duration, command_type, &amps, description);

        if (parsed < 3) continue;  /* skip malformed lines */

        /* Stop when we hit the END marker */
        if (strcmp(command_type, "END") == 0) break;

        /* Build the command struct to send */
        struct bess_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.param = amps;
        strncpy(cmd.label, description, sizeof(cmd.label) - 1);

        if      (strcmp(command_type, "CHARGE")    == 0) cmd.cmd = CMD_CHARGE;
        else if (strcmp(command_type, "DISCHARGE") == 0) cmd.cmd = CMD_DISCHARGE;
        else                                              cmd.cmd = CMD_IDLE;

        /* Send the command to the message queue.
         * mq_send args: queue, data, size, priority
         * PRIO_CMD = 1 (normal priority — TRIP messages at 5 will jump ahead) */
        fprintf(stderr, "[sched] sending %s %.1fA (%s)\n",
                command_type, amps, description);

        if (mq_send(mq, (char *)&cmd, sizeof(cmd), PRIO_CMD) < 0)
            perror("mq_send");

        /* Wait for this step's duration before sending the next command */
        sleep((unsigned int)duration);
    }

    fclose(fp);
    mq_close(mq);
    fprintf(stderr, "[sched] profile complete\n");
    return 0;
}
