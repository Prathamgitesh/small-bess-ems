/*
 * supervisor.c
 * ============
 * This is the FIRST process that runs. It does 3 things:
 *
 *   1. Creates all IPC objects (shared memory, semaphore, message queue, FIFO, pipe)
 *   2. Starts all 5 child processes (simulator, scheduler, protection, logger, dashboard)
 *   3. Watches them — if one dies, it restarts it automatically
 *
 * Think of it like a restaurant manager:
 *   - Manager sets up the kitchen equipment (IPC objects)
 *   - Manager hires staff (fork + execv)
 *   - Manager notices if a staff member leaves and hires a replacement (SIGCHLD + restart)
 *
 * KEY LINUX CONCEPTS HERE:
 *   fork()       - creates a copy of this process (a "child")
 *   execv()      - inside child: replace the child with a different program
 *   sigaction()  - register a function to run when a signal arrives
 *   waitpid()    - check if a child process has exited
 *   alarm()      - set a timer; SIGALRM fires after N seconds
 *   pipe()       - create a one-way communication channel to logger
 *   shm_open()   - create a shared memory object
 *   sem_open()   - create a semaphore (a lock)
 *   mq_open()    - create a message queue
 *   mkfifo()     - create a named pipe (FIFO)
 */

#define _POSIX_C_SOURCE 200809L  /* tells compiler to enable POSIX functions */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      /* fork, execv, pipe, alarm, sleep, close       */
#include <fcntl.h>       /* O_CREAT, O_RDWR etc.                          */
#include <signal.h>      /* sigaction, SIGCHLD, SIGTERM etc.              */
#include <sys/wait.h>    /* waitpid, WNOHANG                              */
#include <sys/mman.h>    /* shm_open concept (used with mmap in others)   */
#include <sys/stat.h>    /* mkfifo, ftruncate                             */
#include <semaphore.h>   /* sem_open, sem_close, sem_unlink               */
#include <mqueue.h>      /* mq_open, mq_close, mq_unlink                 */
#include "../include/ipc_config.h"
#include "../include/bess_types.h"


/* ==========================================================================
 * Child process table
 * --------------------------------------------------------------------------
 * We track each child by its executable path and current PID.
 * PID = 0 means "this child is not running right now".
 * ========================================================================== */
struct child_info {
    const char *path;   /* path to the executable, e.g. "./simulator" */
    pid_t       pid;    /* PID of the running child (0 if not running) */
};

static struct child_info children[] = {
    { "./simulator",  0 },
    { "./scheduler",  0 },
    { "./protection", 0 },
    { "./logger",     0 },
    { "./dashboard",  0 },
};
#define N_CHILDREN 5  /* how many children we manage */


/* ==========================================================================
 * The pipe: supervisor → logger
 * --------------------------------------------------------------------------
 * pipe_fd[0] = read end  (logger reads from this)
 * pipe_fd[1] = write end (supervisor writes to this)
 * ========================================================================== */
static int pipe_fd[2];

/* These flags are set by signal handlers to tell the main loop what happened */
static volatile sig_atomic_t g_running = 1;  /* 1 = keep running, 0 = shut down */
static volatile sig_atomic_t g_trip    = 0;  /* 1 = SIGUSR1 received = manual trip */


/* ==========================================================================
 * Signal handlers
 * --------------------------------------------------------------------------
 * Signal handlers must be very short. They just set a flag.
 * The actual work is done in the main loop after the signal is delivered.
 *
 * Why volatile sig_atomic_t?
 *   Because the signal can arrive at any moment (even between two instructions).
 *   sig_atomic_t guarantees the write is atomic (not half-done).
 * ========================================================================== */

/* Called when a child process exits — we need to reap it with waitpid */
static void on_sigchld(int s) { (void)s; /* just wakes up pause() */ }

/* Called every WATCHDOG_S seconds — we can check if simulator is still alive */
static void on_sigalrm(int s) { (void)s; /* reset the alarm in the main loop */ }

/* Called when user presses Ctrl+C or when we get SIGTERM */
static void on_term(int s)    { (void)s; g_running = 0; }

/* Called when someone sends: kill -USR1 <supervisor_pid>  →  manual TRIP */
static void on_sigusr1(int s) { (void)s; g_trip = 1; }


/* ==========================================================================
 * spawn() — start one child process
 * --------------------------------------------------------------------------
 * Steps:
 *   1. fork() — creates a copy of supervisor
 *   2. In the child copy:
 *      - If it's logger, wire up the pipe to stdin so logger reads from pipe
 *      - execv() — replace this child copy with the actual executable
 *   3. In the parent (supervisor):
 *      - Save the child's PID so we can track it
 * ========================================================================== */
static void spawn(struct child_info *c)
{
    pid_t pid = fork();  /* after this line, TWO processes are running */

    if (pid < 0) {
        perror("fork failed");
        return;
    }

    if (pid == 0) {
        /*
         * We are inside the CHILD process now.
         * Everything we do here only affects the child.
         */

        /* Special case for logger: connect pipe read end to stdin */
        if (strcmp(c->path, "./logger") == 0) {
            close(pipe_fd[1]);              /* child doesn't need write end   */
            dup2(pipe_fd[0], STDIN_FILENO); /* pipe read end → stdin (fd 0)   */
            close(pipe_fd[0]);              /* original fd no longer needed   */
        }

        /* Replace this child process with the actual program */
        char *argv[] = { (char *)c->path, NULL };  /* argv[0] = program name, NULL = end */
        execv(c->path, argv);

        /* execv() never returns if it succeeds.
         * If we reach here, it failed (e.g. file not found). */
        perror("execv failed");
        _exit(1);  /* use _exit (not exit) in child after fork */
    }

    /* We are in the PARENT (supervisor).
     * pid is the PID of the newly created child. */
    c->pid = pid;
    fprintf(stderr, "[sup] started %s (PID=%d)\n", c->path, pid);
}


int main(void)
{
    fprintf(stderr, "[sup] supervisor starting (PID=%d)\n", getpid());

    /* ------------------------------------------------------------------
     * STEP 1: Install signal handlers using sigaction()
     *
     * Why sigaction() and not signal()?
     *   signal() behaviour is different across systems.
     *   sigaction() is the modern, reliable way on Linux.
     *
     * sa_handler = the function to call when the signal arrives
     * sigemptyset = don't block any other signals while handler runs
     * sa_flags = 0 means: don't restart syscalls after signal (important!)
     * ------------------------------------------------------------------ */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = on_sigchld;  sigaction(SIGCHLD,  &sa, NULL);
    sa.sa_handler = on_sigalrm;  sigaction(SIGALRM,  &sa, NULL);
    sa.sa_handler = on_term;     sigaction(SIGTERM,  &sa, NULL);
    sa.sa_handler = on_term;     sigaction(SIGINT,   &sa, NULL);
    sa.sa_handler = on_sigusr1;  sigaction(SIGUSR1,  &sa, NULL);

    /* ------------------------------------------------------------------
     * STEP 2: Create all IPC objects
     *
     * IMPORTANT: Only supervisor creates these. All other processes
     * just open/attach to them. This is like the manager setting up
     * the tools before the staff arrive.
     * ------------------------------------------------------------------ */

    /* Anonymous pipe — supervisor writes, logger reads via stdin */
    if (pipe(pipe_fd) < 0) { perror("pipe"); exit(1); }

    /* Shared memory — a block of memory all processes can read/write */
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) { perror("shm_open"); exit(1); }
    /* Set the size — must be done before anyone mmap()s it */
    ftruncate(shm_fd, sizeof(struct bess_state));
    close(shm_fd);  /* fd not needed after sizing */

    /* Semaphore — acts as a lock (initial value 1 = unlocked) */
    sem_t *sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) { perror("sem_open"); exit(1); }
    sem_close(sem);  /* we don't need to hold the handle here */

    /* Message queue — for sending commands to simulator */
    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg  = MQ_MAXMSG;
    attr.mq_msgsize = MQ_MSGSIZE;
    mqd_t mq = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0666, &attr);
    if (mq == (mqd_t)-1) { perror("mq_open"); exit(1); }
    mq_close(mq);

    /* Named FIFO — simulator writes JSON here, dashboard reads it */
    mkfifo(FIFO_PATH, 0666);  /* if it already exists, that's fine */

    /* ------------------------------------------------------------------
     * STEP 3: Spawn all children
     * ------------------------------------------------------------------ */
    for (int i = 0; i < N_CHILDREN; i++)
        spawn(&children[i]);

    /* Close our copy of the pipe read end — only logger needs it */
    close(pipe_fd[0]);

    /* ------------------------------------------------------------------
     * STEP 4: Start watchdog timer
     * SIGALRM will fire after WATCHDOG_S seconds
     * ------------------------------------------------------------------ */
    alarm(WATCHDOG_S);

    /* ------------------------------------------------------------------
     * STEP 5: Main supervisor loop
     *
     * pause() puts this process to sleep until a signal arrives.
     * When any signal fires, pause() returns and we handle it.
     * ------------------------------------------------------------------ */
    while (g_running) {

        /* Check for dead children without blocking.
         * WNOHANG means "return immediately if no child has exited" */
        pid_t dead;
        while ((dead = waitpid(-1, NULL, WNOHANG)) > 0) {
            for (int i = 0; i < N_CHILDREN; i++) {
                if (children[i].pid == dead) {
                    fprintf(stderr, "[sup] %s died — restarting\n", children[i].path);
                    children[i].pid = 0;
                    spawn(&children[i]);  /* restart the dead child */
                }
            }
        }

        /* If SIGUSR1 was received, send a manual TRIP command */
        if (g_trip) {
            g_trip = 0;
            fprintf(stderr, "[sup] SIGUSR1 received — sending manual TRIP\n");

            struct bess_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.cmd   = CMD_TRIP;
            cmd.param = (double)FAULT_OVERCURRENT;
            snprintf(cmd.label, sizeof(cmd.label), "manual SIGUSR1 trip");

            mqd_t m = mq_open(MQ_NAME, O_WRONLY);
            mq_send(m, (char *)&cmd, sizeof(cmd), PRIO_TRIP);
            mq_close(m);
        }

        /* Reset the watchdog timer */
        alarm(WATCHDOG_S);

        /* Sleep until the next signal */
        pause();
    }

    /* ------------------------------------------------------------------
     * STEP 6: Graceful shutdown
     * Send SIGTERM to all children and wait for them to exit cleanly.
     * ------------------------------------------------------------------ */
    fprintf(stderr, "[sup] shutting down\n");

    for (int i = 0; i < N_CHILDREN; i++) {
        if (children[i].pid > 0) {
            kill(children[i].pid, SIGTERM);  /* ask child to exit */
        }
    }

    /* Wait for ALL children to exit */
    while (waitpid(-1, NULL, 0) > 0);

    /* ------------------------------------------------------------------
     * STEP 7: Clean up IPC objects
     * These exist on the filesystem until explicitly deleted.
     * ------------------------------------------------------------------ */
    shm_unlink(SHM_NAME);   /* delete shared memory */
    sem_unlink(SEM_NAME);   /* delete semaphore      */
    mq_unlink(MQ_NAME);     /* delete message queue  */
    unlink(FIFO_PATH);      /* delete named FIFO     */
    close(pipe_fd[1]);      /* close write end of pipe */

    fprintf(stderr, "[sup] done\n");
    return 0;
}
