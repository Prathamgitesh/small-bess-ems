/*
 * simulator.c
 * ===========
 * This process IS the battery. It:
 *   - Keeps track of SoC (State of Charge) and Voltage
 *   - Runs 2 threads that update these values every second
 *   - Writes the current state to shared memory (so others can read it)
 *   - Sends a JSON line to the FIFO every second (so dashboard can display it)
 *   - Listens on the message queue for commands (charge/discharge/stop)
 *
 * Think of it like the battery hardware itself with a built-in sensor:
 *   - Thread 1 (soc_thread):       updates SoC and Voltage every second
 *   - Thread 2 (telemetry_thread): publishes the state so others can read it
 *   - Main thread:                 receives and applies commands
 *
 * KEY LINUX CONCEPTS HERE:
 *   pthread_create()         - create a new thread
 *   pthread_join()           - wait for a thread to finish
 *   pthread_mutex_lock()     - lock a mutex (so two threads don't write at same time)
 *   pthread_mutex_unlock()   - unlock the mutex
 *   pthread_cond_signal()    - wake up a thread that is waiting
 *   pthread_cond_wait()      - sleep until another thread signals you
 *   mq_receive()             - receive a message from the queue (blocks until one arrives)
 *   sem_wait() / sem_post()  - lock/unlock the shared memory window
 *   mmap()                   - attach to shared memory
 *   write()                  - write to FIFO
 *   nanosleep()              - sleep for a precise amount of time
 *   sigaction()              - register signal handler
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>        /* fabs, fmax, fmin  */
#include <time.h>        /* time()            */
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>     /* all pthread_* functions */
#include <semaphore.h>
#include <mqueue.h>
#include <sys/mman.h>
#include "../include/ipc_config.h"
#include "../include/bess_types.h"


/* ==========================================================================
 * Global variables shared between threads
 * --------------------------------------------------------------------------
 * All threads in this process share the same memory.
 * We must protect these with a mutex to prevent "race conditions"
 * (two threads writing at the same time = corrupted data).
 * ========================================================================== */

/* The one and only copy of battery state in this process */
static struct bess_state g_state;

/*
 * Mutex: like a toilet lock — only one thread can hold it at a time.
 * PTHREAD_MUTEX_INITIALIZER is a shortcut to initialise without calling init().
 */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Condition variable: lets one thread wake up another thread.
 * soc_thread will "signal" this after each update.
 * telemetry_thread will "wait" on this until it gets signaled.
 */
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

/* Predicate: 1 = new data is ready, 0 = already published */
static int g_new_data = 0;

/* IPC handles (opened in main, used by threads) */
static struct bess_state *g_shm  = NULL;  /* pointer to shared memory       */
static sem_t             *g_sem  = NULL;  /* semaphore handle               */
static mqd_t              g_mq   = (mqd_t)-1;  /* message queue descriptor  */
static int                g_fifo = -1;          /* FIFO file descriptor      */

/* Shutdown flag — sig_atomic_t is safe to write from signal handler */
static volatile sig_atomic_t g_running = 1;

static void on_term(int s) { (void)s; g_running = 0; }


/* ==========================================================================
 * OCV lookup table (Open Circuit Voltage from State of Charge)
 * --------------------------------------------------------------------------
 * A real LFP battery's voltage depends on its SoC.
 * We use a simple 5-point table and interpolate between them.
 *
 * Example: SoC=0.5 → voltage ≈ 204.8V
 *          SoC=0.9 → voltage ≈ 220.8V
 *
 * No internal resistance in this simplified version.
 * So: voltage = OCV(soc) exactly.
 * ========================================================================== */
static double get_voltage_from_soc(double soc)
{
    /* Breakpoints: what SoC values do we have voltage data for? */
    static const double soc_points[] = { 0.0,   0.1,   0.5,   0.9,   1.0   };
    static const double volt_points[] = { 179.2, 192.0, 204.8, 220.8, 230.4 };

    /* Clamp to [0, 1] range */
    if (soc <= 0.0) return 179.2;
    if (soc >= 1.0) return 230.4;

    /* Find which two breakpoints soc falls between, then interpolate */
    for (int i = 0; i < 4; i++) {
        if (soc <= soc_points[i+1]) {
            /*
             * Linear interpolation formula:
             *   t = how far between soc_points[i] and soc_points[i+1]
             *   result = volt_points[i] + t * (volt_points[i+1] - volt_points[i])
             */
            double t = (soc - soc_points[i]) / (soc_points[i+1] - soc_points[i]);
            return volt_points[i] + t * (volt_points[i+1] - volt_points[i]);
        }
    }
    return 204.8;  /* fallback (should never reach here) */
}


/* ==========================================================================
 * THREAD 1: soc_thread
 * --------------------------------------------------------------------------
 * Runs forever at 1 Hz (once per second).
 *
 * Each tick:
 *   1. Lock the mutex (take the lock)
 *   2. Read current setpoint
 *   3. Update SoC using Coulomb counting formula
 *   4. Update voltage using the OCV lookup table
 *   5. Set g_new_data = 1 and signal telemetry_thread
 *   6. Unlock the mutex
 *
 * COULOMB COUNTING:
 *   SoC changes based on how much charge flows in or out.
 *   Formula: delta_SoC = (current × time) / (3600 × capacity)
 *   Example: 50A charge for 30 simulated seconds on 100Ah battery:
 *            delta_SoC = (50 × 30) / (3600 × 100) = 0.00417  (+0.417%)
 * ========================================================================== */
static void *soc_thread(void *arg)
{
    (void)arg;  /* we don't use the argument */

    while (g_running) {
        /* Sleep for exactly 1 second */
        nanosleep(&(struct timespec){1, 0}, NULL);

        /* Lock the mutex before touching g_state */
        pthread_mutex_lock(&g_mutex);

        /* Only update physics if battery is actively charging/discharging */
        if (g_state.mode == MODE_CHARGE || g_state.mode == MODE_DISCHARGE) {

            /*
             * Time step:
             * In reality, 1 second passed. But we simulate 30 seconds.
             * SIM_TIME_FACTOR converts real time → simulated time.
             */
            double simulated_seconds = (double)SIM_TIME_FACTOR;

            /* Coulomb counting: how much did SoC change? */
            double delta_soc = (g_state.current * simulated_seconds)
                               / (3600.0 * PACK_CAPACITY_AH);

            /* Apply the change, clamp between 0.0 and 1.0 */
            g_state.soc = g_state.soc + delta_soc;
            if (g_state.soc > 1.0) g_state.soc = 1.0;
            if (g_state.soc < 0.0) g_state.soc = 0.0;

            /* Recalculate voltage from the updated SoC */
            g_state.voltage = get_voltage_from_soc(g_state.soc);
        }

        /* Tell telemetry_thread that new data is ready */
        g_new_data = 1;
        pthread_cond_signal(&g_cond);  /* wake up telemetry_thread */

        pthread_mutex_unlock(&g_mutex);
    }

    return NULL;
}


/* ==========================================================================
 * THREAD 2: telemetry_thread
 * --------------------------------------------------------------------------
 * Waits for soc_thread to produce new data, then publishes it.
 *
 * Each wakeup:
 *   1. Wait on g_cond (this releases the mutex and sleeps)
 *   2. When woken: take a snapshot of g_state
 *   3. Write snapshot to shared memory (under semaphore)
 *   4. Write a JSON line to the FIFO (for dashboard to read)
 *
 * WHY CONDITION VARIABLE INSTEAD OF JUST SLEEPING?
 *   - If soc_thread is slow, telemetry_thread doesn't waste CPU busy-waiting
 *   - The two threads stay perfectly in sync
 *
 * WHY "while (!g_new_data)" instead of "if (!g_new_data)"?
 *   POSIX allows "spurious wakeups" — cond_wait can wake up for NO reason.
 *   The while loop re-checks and goes back to sleep if nothing is ready.
 * ========================================================================== */
static void *telemetry_thread(void *arg)
{
    (void)arg;
    struct bess_state snapshot;  /* local copy of state to publish */

    /* We need the mutex locked to call pthread_cond_wait */
    pthread_mutex_lock(&g_mutex);

    while (g_running) {

        /* Sleep here until soc_thread calls pthread_cond_signal
         * pthread_cond_wait does TWO things atomically:
         *   1. Releases g_mutex  (so soc_thread can lock it and update state)
         *   2. Puts this thread to sleep
         * When woken, it automatically re-acquires g_mutex before returning */
        while (g_new_data == 0 && g_running) {
            pthread_cond_wait(&g_cond, &g_mutex);
        }

        if (!g_running) break;

        g_new_data = 0;           /* mark data as consumed */
        snapshot = g_state;       /* copy current state while mutex is held */
        snapshot.last_update = time(NULL);

        /* Release mutex before doing I/O (don't hold mutex while writing) */
        pthread_mutex_unlock(&g_mutex);

        /* ----------------------------------------------------------------
         * Write to shared memory (protected by semaphore)
         *
         * Semaphore usage:
         *   sem_wait(g_sem): if value>0, decrement and continue (we got the lock)
         *                    if value=0, BLOCK until someone calls sem_post
         *   sem_post(g_sem): increment value (release the lock)
         *
         * This prevents protection.c from reading half-written data.
         * ---------------------------------------------------------------- */
        sem_wait(g_sem);             /* lock shared memory                  */
        snapshot.tick = g_shm->tick + 1;
        *g_shm = snapshot;           /* copy entire struct into shared memory */
        sem_post(g_sem);             /* unlock shared memory                */

        /* ----------------------------------------------------------------
         * Write JSON line to FIFO → dashboard will read and display this
         * ---------------------------------------------------------------- */
        if (g_fifo >= 0) {
            char buf[256];
            int n = snprintf(buf, sizeof(buf),
                "{\"soc\":%.3f,\"v\":%.2f,\"i\":%.1f,\"mode\":%d,\"fault\":%d}\n",
                snapshot.soc,
                snapshot.voltage,
                snapshot.current,
                snapshot.mode,
                snapshot.fault_code);
            write(g_fifo, buf, n);
        }

        /* Re-lock mutex before looping back to cond_wait */
        pthread_mutex_lock(&g_mutex);
    }

    pthread_mutex_unlock(&g_mutex);
    return NULL;
}


int main(void)
{
    fprintf(stderr, "[sim] simulator starting (PID=%d)\n", getpid());

    /* ------------------------------------------------------------------
     * Signal handler: no SA_RESTART flag.
     *
     * Why no SA_RESTART?
     * When SIGTERM arrives, mq_receive() is blocking.
     * Without SA_RESTART, the signal interrupts mq_receive()
     * and it returns -1 with errno=EINTR.
     * Our main loop checks errno==EINTR and then checks g_running.
     * This is how we cleanly exit from a blocking call.
     * ------------------------------------------------------------------ */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    /* sa.sa_flags = 0  ← no SA_RESTART on purpose */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* ------------------------------------------------------------------
     * Attach to IPC objects (supervisor already created all of these)
     * ------------------------------------------------------------------ */

    /* Shared memory: shm_open gives a file descriptor, mmap maps it into memory */
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (shm_fd < 0) { perror("shm_open"); exit(1); }
    g_shm = mmap(NULL,                    /* let kernel choose address     */
                 sizeof(struct bess_state),
                 PROT_READ | PROT_WRITE,  /* we need to read AND write     */
                 MAP_SHARED,              /* share with other processes    */
                 shm_fd, 0);
    if (g_shm == MAP_FAILED) { perror("mmap"); exit(1); }
    close(shm_fd);  /* fd can be closed after mmap; the mapping stays */

    /* Semaphore */
    g_sem = sem_open(SEM_NAME, 0);  /* 0 = don't create, just open */
    if (g_sem == SEM_FAILED) { perror("sem_open"); exit(1); }

    /* Message queue (we only receive, so O_RDONLY) */
    g_mq = mq_open(MQ_NAME, O_RDONLY);
    if (g_mq == (mqd_t)-1) { perror("mq_open"); exit(1); }

    /* FIFO: open the write end.
     * This BLOCKS here until dashboard.c opens the read end.
     * Both sides must be open before either can use it. */
    fprintf(stderr, "[sim] waiting for dashboard to open FIFO...\n");
    g_fifo = open(FIFO_PATH, O_WRONLY);
    if (g_fifo < 0) {
        perror("open FIFO (warning)");
        /* non-fatal: we just won't send telemetry */
    }
    fprintf(stderr, "[sim] FIFO open, starting\n");

    /* ------------------------------------------------------------------
     * Set initial battery state
     * ------------------------------------------------------------------ */
    memset(&g_state, 0, sizeof(g_state));
    g_state.soc     = 0.60;                           /* start at 60% SoC  */
    g_state.voltage = get_voltage_from_soc(0.60);     /* ~207V              */
    g_state.current = 0.0;
    g_state.mode    = MODE_IDLE;
    g_state.fault_code = FAULT_NONE;

    fprintf(stderr, "[sim] initial: SoC=%.0f%% V=%.2fV\n",
            g_state.soc * 100.0, g_state.voltage);

    /* ------------------------------------------------------------------
     * Create the 2 threads
     * ------------------------------------------------------------------ */
    pthread_t t_soc, t_telem;

    /* pthread_create args: &thread_id, attributes(NULL=default), function, argument */
    if (pthread_create(&t_soc,   NULL, soc_thread,       NULL) != 0)
        { perror("create soc_thread"); exit(1); }
    if (pthread_create(&t_telem, NULL, telemetry_thread, NULL) != 0)
        { perror("create telem_thread"); exit(1); }

    /* ------------------------------------------------------------------
     * Main thread: Command receiver loop
     *
     * mq_receive() BLOCKS until a message arrives in the queue.
     * When a message arrives, we update g_state to apply the command.
     *
     * Note: we use a char buffer of size MQ_MSGSIZE to receive into,
     * then copy into struct bess_cmd. This is required because mq_receive
     * needs a buffer >= the queue's mq_msgsize attribute.
     * ------------------------------------------------------------------ */
    char recv_buf[MQ_MSGSIZE];  /* raw receive buffer */
    struct bess_cmd cmd;

    while (g_running) {

        ssize_t n = mq_receive(g_mq, recv_buf, MQ_MSGSIZE, NULL);

        if (n < 0) {
            if (errno == EINTR) {
                /* A signal interrupted us — loop back to check g_running */
                continue;
            }
            perror("mq_receive");
            continue;
        }

        /* Copy raw bytes into our struct */
        memcpy(&cmd, recv_buf, sizeof(cmd));

        /* Apply the command by updating g_state under the mutex */
        pthread_mutex_lock(&g_mutex);

        switch (cmd.cmd) {

            case CMD_CHARGE:
                g_state.mode    = MODE_CHARGE;
                g_state.current = +fabs(cmd.param);  /* force positive (charging) */
                fprintf(stderr, "[sim] CHARGE %.1fA — %s\n", cmd.param, cmd.label);
                break;

            case CMD_DISCHARGE:
                g_state.mode    = MODE_DISCHARGE;
                g_state.current = -fabs(cmd.param);  /* force negative (discharging) */
                fprintf(stderr, "[sim] DISCHARGE %.1fA — %s\n", cmd.param, cmd.label);
                break;

            case CMD_IDLE:
                g_state.mode    = MODE_IDLE;
                g_state.current = 0.0;
                fprintf(stderr, "[sim] IDLE\n");
                break;

            case CMD_TRIP:
                /* Emergency stop — set fault, zero current, lock out further ops */
                g_state.mode       = MODE_FAULT;
                g_state.current    = 0.0;
                g_state.fault_code = (int)cmd.param;  /* which fault? */
                fprintf(stderr, "[sim] TRIP! fault_code=%d — %s\n",
                        g_state.fault_code, cmd.label);
                break;

            case CMD_RESET:
                /* Clear the fault and go back to idle */
                g_state.mode       = MODE_IDLE;
                g_state.fault_code = FAULT_NONE;
                fprintf(stderr, "[sim] RESET — back to idle\n");
                break;
        }

        pthread_mutex_unlock(&g_mutex);
    }

    /* ------------------------------------------------------------------
     * Shutdown: wake telemetry_thread so it sees g_running=0 and exits
     * ------------------------------------------------------------------ */
    fprintf(stderr, "[sim] shutting down\n");
    pthread_mutex_lock(&g_mutex);
    g_new_data = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);

    /* Wait for both threads to finish */
    pthread_join(t_soc,   NULL);
    pthread_join(t_telem, NULL);

    /* Clean up */
    if (g_fifo >= 0) close(g_fifo);
    mq_close(g_mq);
    sem_close(g_sem);
    munmap(g_shm, sizeof(struct bess_state));

    fprintf(stderr, "[sim] done\n");
    return 0;
}
