/* scheduler.c — reads load profile, sends commands on MQ
 *
 * SYSCALLS / CONCEPTS USED HERE:
 *   mq_open()        attach to existing message queue (write-only)
 *   mq_send()        send one bess_cmd_t to simulator
 *   fopen/fgets      read load_profile.txt line by line
 *   sscanf()         parse each line into fields
 *   sleep()          wait the step's duration before sending next command
 *   sigaction()      clean exit on SIGTERM
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

int main(void) {
    struct sigaction sa = { .sa_handler = on_term };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    mqd_t mq = mq_open(MQ_NAME, O_WRONLY);          /* attach, don't create */
    if (mq == (mqd_t)-1) { perror("mq_open"); exit(1); }

    FILE *fp = fopen("data/load_profile.txt", "r");
    if (!fp) { perror("fopen"); exit(1); }

    /*
     * load_profile.txt format:
     *   30  CHARGE     50  Off-peak charge
     *   30  DISCHARGE  30  Morning discharge
     *   10  DISCHARGE 110  Overcurrent spike  ← protection trips this
     *   30  IDLE        0  Recovery
     *    0  END         0  Done
     */
    char line[128];
    while (g_running && fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        int dur; char type[16]; double amps; char label[48];
        sscanf(line, "%d %15s %lf %47[^\n]", &dur, type, &amps, label);

        if (strcmp(type, "END") == 0) break;

        /* build and send the command */
        bess_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        if      (strcmp(type, "CHARGE")    == 0) cmd.cmd = CMD_CHARGE;
        else if (strcmp(type, "DISCHARGE") == 0) cmd.cmd = CMD_DISCHARGE;
        else                                      cmd.cmd = CMD_IDLE;
        cmd.param = amps;
        strncpy(cmd.label, label, sizeof(cmd.label)-1);

        mq_send(mq, (char*)&cmd, sizeof(cmd), PRIO_CMD); /* enqueue command */

        sleep((unsigned)dur);                    /* wait this step's duration */
    }

    fclose(fp);
    mq_close(mq);
    return 0;
}
