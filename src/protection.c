/* protection.c — polls shared memory, sends TRIP if threshold crossed
 *
 * SYSCALLS / CONCEPTS USED HERE:
 *   shm_open()       attach to shared memory (READ-ONLY)
 *   mmap()           map shm into address space
 *   sem_wait/post    take/release shm lock before reading snapshot
 *   mq_open()        attach to message queue (write-only)
 *   mq_send()        send CMD_TRIP at high priority (PRIO_TRIP = 5)
 *   nanosleep()      1 Hz poll interval
 *   sigaction()      clean exit
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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

int main(void) {
    struct sigaction sa = { .sa_handler = on_term };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    /* attach to shm read-only */
    int fd = shm_open(SHM_NAME, O_RDONLY, 0);
    bess_state_t *shm = mmap(NULL, sizeof(bess_state_t),
                             PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    sem_t *sem = sem_open(SEM_NAME, 0);
    mqd_t  mq  = mq_open(MQ_NAME, O_WRONLY);

    while (g_running) {
        nanosleep(&(struct timespec){1, 0}, NULL);   /* poll at 1 Hz          */

        /* read snapshot under semaphore */
        sem_wait(sem);
        bess_state_t snap = *shm;                    /* struct copy           */
        sem_post(sem);

        /* only check when pack is active */
        if (snap.mode == MODE_FAULT || snap.mode == MODE_IDLE) continue;

        /* threshold check — just overcurrent in this project */
        if (fabs(snap.current) > PROT_OVERCURRENT_A) {
            bess_cmd_t cmd = { .cmd=CMD_TRIP, .param=FAULT_OVERCURRENT };
            snprintf(cmd.label, sizeof(cmd.label),
                     "overcurrent %.1fA", fabs(snap.current));

            /* PRIO_TRIP = 5, higher than scheduler's PRIO_CMD = 1
               so this message jumps the queue and arrives first      */
            mq_send(mq, (char*)&cmd, sizeof(cmd), PRIO_TRIP);

            fprintf(stderr, "[prot] TRIP: %.1fA > %.1fA threshold\n",
                    fabs(snap.current), PROT_OVERCURRENT_A);
        }
    }

    mq_close(mq);
    sem_close(sem);
    munmap(shm, sizeof(bess_state_t));
    return 0;
}
