/* logger.c — reads pipe from supervisor, writes binary log file
 *
 * SYSCALLS / CONCEPTS USED HERE:
 *   read()           read pipe_msg_t structs from stdin (= pipe read end)
 *                    stdin was wired to the pipe by supervisor's dup2()
 *   open()           create/open binary log file
 *   write()          append log_record_t to file
 *   lseek()          jump back to header to update record_count
 *                    then seek to end to resume appending
 *   close()          close file on exit
 *   sigaction()      clean exit
 *
 * BINARY LOG LAYOUT:
 *   [log_header_t  — 20 bytes]
 *   [log_record_t  — record 0]
 *   [log_record_t  — record 1]
 *   ...
 *   record_count in header is updated every write via lseek(SEEK_SET)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stddef.h>
#include "../include/ipc_config.h"
#include "../include/bess_types.h"

static volatile sig_atomic_t g_running = 1;
static void on_term(int s) { (void)s; g_running = 0; }

int main(void) {
    struct sigaction sa = { .sa_handler = on_term };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    /* open binary log file */
    int log_fd = open("bess.log", O_CREAT|O_RDWR|O_TRUNC, 0644);
    if (log_fd < 0) { perror("open log"); exit(1); }

    /* write initial header */
    log_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, "BESS", 4);
    hdr.session_start = time(NULL);
    hdr.record_count  = 0;
    write(log_fd, &hdr, sizeof(hdr));           /* write header at offset 0  */

    /* stdin = pipe read end (supervisor did dup2 before execv) */
    pipe_msg_t msg;
    while (g_running) {
        ssize_t n = read(STDIN_FILENO, &msg, sizeof(msg));
        if (n <= 0) break;                      /* pipe closed = supervisor exited */

        /* build log record from pipe message */
        log_record_t rec;
        rec.timestamp = msg.timestamp;
        rec.event     = msg.event;
        rec.soc       = msg.soc;
        rec.voltage   = msg.voltage;
        rec.current   = msg.current;
        rec.mode      = msg.mode;
        strncpy(rec.msg, msg.msg, sizeof(rec.msg)-1);

        /* append record at current file position */
        write(log_fd, &rec, sizeof(rec));

        /* lseek back to record_count field in header and update it */
        hdr.record_count++;
        lseek(log_fd, offsetof(log_header_t, record_count), SEEK_SET);
        write(log_fd, &hdr.record_count, sizeof(hdr.record_count));

        /* lseek back to end of file to resume appending */
        lseek(log_fd, 0, SEEK_END);
    }

    close(log_fd);
    return 0;
}
