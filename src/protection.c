/*
 * protection.c
 * ============
 * Watches the battery state (via shared memory) and sends an emergency
 * TRIP command if any value crosses a danger threshold.
 *
 * Think of it like a circuit breaker:
 *   - It watches the current every second
 *   - If current > 100A → TRIP (send emergency stop)
 *
 * It reads shared memory (NOT writes) so it doesn't change anything —
 * it's purely an observer that can pull the emergency brake.
 *
 * KEY LINUX CONCEPTS HERE:
 *   shm_open()   - open shared memory (READ-ONLY this time)
 *   mmap()       - map shared memory into our address space
 *   sem_wait()   - lock the shared memory before reading
 *   sem_post()   - unlock after reading
 *   mq_open()    - open message queue for writing
 *   mq_send()    - send TRIP at HIGH priority (PRIO_TRIP=5)
 *   nanosleep()  - poll every 1 second
 *   sigaction()  - clean exit
 *
 * WHY semaphore even for reading?
 *   Without it, we might read g_shm while simulator is half-way through
 *   writing. We'd see a mix of old and new values = garbage data.
 *   The semaphore ensures the write is complete before we read.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>        /* fabs() */
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <semaphore.h>
#include <mqueue.h>
#include <sys/mman.h>
#include "../include/ipc_config.h"
#include "../include/bess_types.h"

static volatile sig_atomic_t g_running = 1;
static void on_term(int s) { (void)s; g_running = 0; }

int main(void)
{
    fprintf(stderr, "[prot] protection starting (PID=%d)\n", getpid());

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    /* Open shared memory READ-ONLY (PROT_READ only in mmap) */
    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (shm_fd < 0) { perror("shm_open"); exit(1); }

    struct bess_state *shm = mmap(NULL,
                                  sizeof(struct bess_state),
                                  PROT_READ,    /* read-only */
                                  MAP_SHARED,
                                  shm_fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); exit(1); }
    close(shm_fd);

    /* Open semaphore — just attach, don't create */
    sem_t *sem = sem_open(SEM_NAME, 0);
    if (sem == SEM_FAILED) { perror("sem_open"); exit(1); }

    /* Open message queue for writing (to send TRIP command) */
    mqd_t mq = mq_open(MQ_NAME, O_WRONLY);
    if (mq == (mqd_t)-1) { perror("mq_open"); exit(1); }

    /* Poll loop: check thresholds every 1 second */
    while (g_running) {
        nanosleep(&(struct timespec){1, 0}, NULL);  /* wait 1 second */

        /* Take a consistent snapshot of shared memory under the semaphore.
         *
         * struct copy: "struct bess_state snap = *shm" copies the entire
         * struct at once (local stack copy), so we can release the
         * semaphore quickly and then check values at our leisure.
         */
        sem_wait(sem);
        struct bess_state snap = *shm;   /* copy the whole struct */
        sem_post(sem);

        /* Don't check thresholds if battery isn't active */
        if (snap.mode == MODE_IDLE || snap.mode == MODE_FAULT) continue;

        /* ---- Threshold check ---- */

        /* fabs() = absolute value (handles both charge and discharge) */
        if (fabs(snap.current) > PROT_OVERCURRENT_A) {

            fprintf(stderr, "[prot] OVERCURRENT! %.1fA > %.1fA — sending TRIP\n",
                    fabs(snap.current), PROT_OVERCURRENT_A);

            /* Build and send the TRIP command */
            struct bess_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.cmd   = CMD_TRIP;
            cmd.param = (double)FAULT_OVERCURRENT;  /* tells simulator which fault */
            snprintf(cmd.label, sizeof(cmd.label),
                     "overcurrent %.1fA", fabs(snap.current));

            /*
             * PRIO_TRIP = 5 (higher than PRIO_CMD = 1)
             * This makes our TRIP message jump ahead of any pending
             * scheduler commands in the queue.
             * The simulator processes highest priority messages first.
             */
            if (mq_send(mq, (char *)&cmd, sizeof(cmd), PRIO_TRIP) < 0)
                perror("mq_send TRIP");
        }
    }

    mq_close(mq);
    sem_close(sem);
    munmap(shm, sizeof(struct bess_state));
    fprintf(stderr, "[prot] done\n");
    return 0;
}
