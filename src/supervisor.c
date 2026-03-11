/* supervisor.c — entry point, spawns all children, owns all IPC objects
 *
 * SYSCALLS USED HERE:
 *   pipe()          create anonymous pipe (supervisor → logger)
 *   fork()          duplicate this process
 *   execv()         replace child with a new binary
 *   sigaction()     install handlers for SIGCHLD, SIGALRM, SIGTERM, SIGUSR1
 *   waitpid()       reap dead children (WNOHANG inside SIGCHLD handler)
 *   alarm()         start watchdog countdown
 *   kill()          send SIGTERM to children on shutdown
 *   shm_open()      CREATE shared memory object
 *   ftruncate()     set shared memory size
 *   sem_open()      CREATE named semaphore (initial value = 1)
 *   mq_open()       CREATE message queue
 *   mkfifo()        CREATE named pipe for telemetry
 *   shm_unlink()    delete shm on exit
 *   sem_unlink()    delete semaphore on exit
 *   mq_unlink()     delete message queue on exit
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <mqueue.h>
#include "../include/ipc_config.h"
#include "../include/bess_types.h"

/* ── child process table ─────────────────────────────────────── */
typedef struct { const char *path; pid_t pid; } child_t;
static child_t children[] = {
    { "./simulator",  0 },
    { "./scheduler",  0 },
    { "./protection", 0 },
    { "./logger",     0 },
    { "./dashboard",  0 },
};
#define N_CHILDREN 5

static int pipe_fd[2];                          /* write end → logger stdin  */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_trip    = 0;

/* ── signal handlers ─────────────────────────────────────────── */
static void on_sigchld(int s) { (void)s; }     /* interrupts pause()        */
static void on_sigalrm(int s) { (void)s; }     /* watchdog fire             */
static void on_term(int s)    { (void)s; g_running = 0; }
static void on_sigusr1(int s) { (void)s; g_trip = 1; }   /* manual TRIP   */

/* ── spawn one child ─────────────────────────────────────────── */
static void spawn(child_t *c) {
    pid_t pid = fork();                         /* duplicate process         */
    if (pid == 0) {
        /* inside child: wire up pipe for logger */
        if (strcmp(c->path, "./logger") == 0) {
            close(pipe_fd[1]);                  /* close write end           */
            dup2(pipe_fd[0], STDIN_FILENO);     /* pipe read end → stdin     */
            close(pipe_fd[0]);
        }
        char *argv[] = { (char *)c->path, NULL };
        execv(c->path, argv);                   /* replace child image       */
        _exit(1);                               /* only reached if execv fails */
    }
    c->pid = pid;
}

int main(void) {
    /* 1. sigaction for every signal we care about */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_sigchld;  sigaction(SIGCHLD,  &sa, NULL);
    sa.sa_handler = on_sigalrm;  sigaction(SIGALRM,  &sa, NULL);
    sa.sa_handler = on_term;     sigaction(SIGTERM,  &sa, NULL);
    sa.sa_handler = on_term;     sigaction(SIGINT,   &sa, NULL);
    sa.sa_handler = on_sigusr1;  sigaction(SIGUSR1,  &sa, NULL);

    /* 2. create IPC — supervisor is the only creator, others just attach */
    pipe(pipe_fd);                              /* anonymous pipe            */

    int fd = shm_open(SHM_NAME, O_CREAT|O_RDWR, 0666);
    ftruncate(fd, sizeof(bess_state_t));        /* set size before mmap      */
    close(fd);

    sem_t *s = sem_open(SEM_NAME, O_CREAT, 0666, 1);  /* value=1 → mutex  */
    sem_close(s);

    struct mq_attr attr = { .mq_maxmsg=MQ_MAXMSG, .mq_msgsize=MQ_MSGSIZE };
    mqd_t mq = mq_open(MQ_NAME, O_CREAT|O_RDWR, 0666, &attr);
    mq_close(mq);

    mkfifo(FIFO_PATH, 0666);                    /* named pipe for telemetry  */

    /* 3. spawn all children */
    for (int i = 0; i < N_CHILDREN; i++) spawn(&children[i]);

    /* 4. watchdog: SIGALRM fires if simulator tick stalls */
    alarm(WATCHDOG_S);

    /* 5. supervisor loop: sleep until a signal wakes us */
    while (g_running) {

        /* reap dead children without blocking */
        pid_t dead;
        while ((dead = waitpid(-1, NULL, WNOHANG)) > 0) {
            for (int i = 0; i < N_CHILDREN; i++) {
                if (children[i].pid == dead) {
                    children[i].pid = 0;
                    spawn(&children[i]);        /* auto-restart              */
                }
            }
        }

        /* manual trip via SIGUSR1 → send CMD_TRIP on MQ */
        if (g_trip) {
            g_trip = 0;
            bess_cmd_t cmd = { .cmd=CMD_TRIP, .param=FAULT_OVERCURRENT };
            snprintf(cmd.label, sizeof(cmd.label), "manual SIGUSR1 trip");
            mqd_t m = mq_open(MQ_NAME, O_WRONLY);
            mq_send(m, (char*)&cmd, sizeof(cmd), PRIO_TRIP);
            mq_close(m);
        }

        pause();                                /* sleep until next signal   */
    }

    /* 6. graceful shutdown */
    for (int i = 0; i < N_CHILDREN; i++)
        if (children[i].pid > 0) kill(children[i].pid, SIGTERM);
    while (waitpid(-1, NULL, 0) > 0);          /* wait for all children     */

    /* 7. remove IPC objects */
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_NAME);
    mq_unlink(MQ_NAME);
    unlink(FIFO_PATH);
    close(pipe_fd[0]); close(pipe_fd[1]);
    return 0;
}
