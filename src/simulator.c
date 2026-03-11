/* simulator.c — battery physics, 2 threads, command receiver
 *
 * SYSCALLS / CONCEPTS USED HERE:
 *   pthread_create()         spawn soc_thread and telemetry_thread
 *   pthread_mutex_lock/unlock protect g_state from concurrent writes
 *   pthread_cond_signal()    soc_thread wakes telemetry_thread each tick
 *   pthread_cond_wait()      telemetry_thread sleeps until signaled
 *   nanosleep()              1 Hz tick inside soc_thread
 *   mq_receive()             main thread blocks here for commands
 *   sem_wait / sem_post      lock/unlock shm window before writing
 *   mmap()                   attach to shared memory created by supervisor
 *   write()                  send JSON line to FIFO (→ dashboard)
 *   sigaction(SIGTERM)       clean exit — NO SA_RESTART so mq_receive
 *                            returns EINTR when signal arrives
 *   pthread_join()           wait for both threads before cleanup
 *
 * PHYSICS (no IR drop, voltage = OCV only):
 *   soc     += (I × dt_sim) / (3600 × C_ah)    ← Coulomb counting
 *   voltage  = OCV(soc)                          ← table lookup
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <mqueue.h>
#include <sys/mman.h>
#include "../include/ipc_config.h"
#include "../include/bess_types.h"

/* ── in-process pack state ───────────────────────────────────── */
static bess_state_t     g_state;
static pthread_mutex_t  g_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cond = PTHREAD_COND_INITIALIZER;
static int              g_tick = 0;            /* predicate for cond_wait   */

/* ── IPC handles ─────────────────────────────────────────────── */
static bess_state_t *g_shm  = NULL;
static sem_t        *g_sem  = NULL;
static mqd_t         g_mq   = (mqd_t)-1;
static int           g_fifo = -1;

static volatile sig_atomic_t g_running = 1;
static void on_term(int s) { (void)s; g_running = 0; }

/* ── minimal OCV table ───────────────────────────────────────── */
static double ocv(double soc) {
    /* 5-point piecewise linear: soc → pack voltage */
    static const double bp[]  = { 0.0,   0.1,   0.5,   0.9,   1.0   };
    static const double ov[]  = { 179.2, 192.0, 204.8, 220.8, 230.4 };
    if (soc <= bp[0]) return ov[0];
    if (soc >= bp[4]) return ov[4];
    for (int i = 0; i < 4; i++) {
        if (soc <= bp[i+1]) {
            double t = (soc - bp[i]) / (bp[i+1] - bp[i]);
            return ov[i] + t * (ov[i+1] - ov[i]);
        }
    }
    return 204.8;
}

/* ══════════════════════════════════════════════════════════════
 * THREAD 1: soc_thread
 *   Runs at 1 Hz (nanosleep).
 *   Each tick: update soc + voltage under mutex, then signal
 *   telemetry_thread via pthread_cond_signal.
 * ════════════════════════════════════════════════════════════ */
static void *soc_thread(void *arg) {
    (void)arg;
    while (g_running) {
        nanosleep(&(struct timespec){1, 0}, NULL);    /* 1 Hz              */

        pthread_mutex_lock(&g_mtx);

        if (g_state.mode != MODE_IDLE && g_state.mode != MODE_FAULT) {
            /* Coulomb counting: ΔSoC = I × dt_sim / (3600 × C_ah) */
            double dt  = (double)SIM_TIME_FACTOR;
            g_state.soc += (g_state.current * dt) / (3600.0 * PACK_CAPACITY_AH);
            g_state.soc  = fmax(0.0, fmin(1.0, g_state.soc));

            /* voltage = OCV(soc), no internal resistance */
            g_state.voltage = ocv(g_state.soc);
        }

        /* wake telemetry_thread */
        g_tick = 1;
        pthread_cond_signal(&g_cond);

        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 * THREAD 2: telemetry_thread
 *   Sleeps on pthread_cond_wait until soc_thread signals.
 *   Wakes, snapshots state, writes to shm (under sem) and FIFO.
 * ════════════════════════════════════════════════════════════ */
static void *telemetry_thread(void *arg) {
    (void)arg;
    bess_state_t snap;

    pthread_mutex_lock(&g_mtx);
    while (g_running) {
        /* spurious-wakeup-safe: loop until predicate is true */
        while (!g_tick && g_running)
            pthread_cond_wait(&g_cond, &g_mtx);
        if (!g_running) break;
        g_tick = 0;

        snap = g_state;                              /* snapshot under mutex */
        snap.last_update = time(NULL);
        pthread_mutex_unlock(&g_mtx);

        /* write to shared memory (sem guards the window) */
        sem_wait(g_sem);
        snap.tick = g_shm->tick + 1;
        *g_shm = snap;                               /* one struct copy      */
        sem_post(g_sem);

        /* write JSON line to FIFO → dashboard reads this */
        char buf[256];
        int n = snprintf(buf, sizeof(buf),
            "{\"soc\":%.3f,\"v\":%.2f,\"i\":%.1f,\"mode\":%d,\"fault\":%d}\n",
            snap.soc, snap.voltage, snap.current, snap.mode, snap.fault_code);
        if (g_fifo >= 0) write(g_fifo, buf, n);

        pthread_mutex_lock(&g_mtx);
    }
    pthread_mutex_unlock(&g_mtx);
    return NULL;
}

int main(void) {
    /* sigaction — no SA_RESTART: SIGTERM will interrupt mq_receive with EINTR */
    struct sigaction sa = { .sa_handler = on_term };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* attach to IPC (supervisor already created all of these) */
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    g_shm  = mmap(NULL, sizeof(bess_state_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);                                       /* fd safe to close after mmap */

    g_sem  = sem_open(SEM_NAME, 0);
    g_mq   = mq_open(MQ_NAME, O_RDONLY);
    g_fifo = open(FIFO_PATH, O_WRONLY);              /* blocks until dashboard opens read end */

    /* initial state */
    memset(&g_state, 0, sizeof(g_state));
    g_state.soc     = 0.60;
    g_state.voltage = ocv(0.60);
    g_state.mode    = MODE_IDLE;

    /* spawn 2 threads */
    pthread_t t_soc, t_telem;
    pthread_create(&t_soc,   NULL, soc_thread,       NULL);
    pthread_create(&t_telem, NULL, telemetry_thread, NULL);

    /* main thread: command dispatch — blocks in mq_receive */
    char buf[MQ_MSGSIZE];
    while (g_running) {
        ssize_t n = mq_receive(g_mq, buf, MQ_MSGSIZE, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;            /* signal → recheck g_running */
            continue;
        }

        bess_cmd_t cmd;
        memcpy(&cmd, buf, sizeof(cmd));

        pthread_mutex_lock(&g_mtx);
        switch (cmd.cmd) {
            case CMD_CHARGE:
                g_state.mode    = MODE_CHARGE;
                g_state.current = +fabs(cmd.param);  /* +ve */
                break;
            case CMD_DISCHARGE:
                g_state.mode    = MODE_DISCHARGE;
                g_state.current = -fabs(cmd.param);  /* −ve */
                break;
            case CMD_IDLE:
                g_state.mode    = MODE_IDLE;
                g_state.current = 0.0;
                break;
            case CMD_TRIP:
                g_state.mode       = MODE_FAULT;
                g_state.current    = 0.0;
                g_state.fault_code = (int)cmd.param;
                break;
            case CMD_RESET:
                g_state.mode       = MODE_IDLE;
                g_state.fault_code = FAULT_NONE;
                break;
        }
        pthread_mutex_unlock(&g_mtx);
    }

    /* wake telemetry_thread so it sees g_running=0 and exits cleanly */
    pthread_mutex_lock(&g_mtx);
    g_tick = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mtx);

    pthread_join(t_soc,   NULL);
    pthread_join(t_telem, NULL);

    /* cleanup */
    close(g_fifo);
    mq_close(g_mq);
    sem_close(g_sem);
    munmap(g_shm, sizeof(bess_state_t));
    return 0;
}
