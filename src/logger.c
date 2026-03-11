/*
 * logger.c
 * ========
 * Reads log events from the anonymous pipe and writes them to a binary file.
 *
 * How supervisor wired this up:
 *   supervisor called dup2(pipe_fd[0], STDIN_FILENO) before execv().
 *   So reading from stdin (fd 0) HERE actually reads from the pipe.
 *   logger doesn't even know about the pipe — it just reads stdin.
 *
 * Why binary file instead of text?
 *   Binary files use lseek() to jump to exact positions.
 *   We store a header at position 0 with record_count.
 *   Every time we add a record, we seek back and update the count.
 *   Try doing that with a text file!
 *
 * FILE LAYOUT:
 *   [struct log_header] ← at byte 0, record_count updated every write
 *   [struct log_record] ← first event
 *   [struct log_record] ← second event
 *   [struct log_record] ← ...
 *
 * KEY LINUX CONCEPTS HERE:
 *   read()    - read from stdin (which is actually the pipe)
 *   open()    - open/create the binary log file
 *   write()   - append a log record
 *   lseek()   - jump to a specific position in the file
 *   close()   - close the file
 *   sigaction() - clean exit
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>   /* offsetof() — tells us where a field is in a struct */
#include <time.h>
#include "../include/ipc_config.h"
#include "../include/bess_types.h"

static volatile sig_atomic_t g_running = 1;
static void on_term(int s) { (void)s; g_running = 0; }

int main(void)
{
    fprintf(stderr, "[log] logger starting (PID=%d)\n", getpid());

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    /* Open the binary log file.
     * O_CREAT = create if it doesn't exist
     * O_RDWR  = open for both reading and writing (lseek needs write access)
     * O_TRUNC = start fresh each session (truncate to 0 bytes if exists)
     * 0644    = file permissions (owner read+write, others read)
     */
    int log_fd = open("bess.log", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (log_fd < 0) {
        perror("open bess.log");
        exit(1);
    }

    /* Write the initial file header at position 0 */
    struct log_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0] = 'B'; hdr.magic[1] = 'E';   /* "BESS" marker to identify file */
    hdr.magic[2] = 'S'; hdr.magic[3] = 'S';
    hdr.record_count  = 0;
    hdr.session_start = time(NULL);
    write(log_fd, &hdr, sizeof(hdr));  /* this positions us right after the header */

    fprintf(stderr, "[log] log file created, waiting for events\n");

    /* Read events from stdin (which is the pipe read end, via dup2) */
    struct pipe_msg msg;

    while (g_running) {

        /*
         * read() blocks until data is available on stdin (the pipe).
         * It returns 0 when the pipe write end is closed (= supervisor exited).
         * We read exactly sizeof(struct pipe_msg) bytes — fixed-size messages
         * prevent partial reads causing misalignment.
         */
        ssize_t n = read(STDIN_FILENO, &msg, sizeof(msg));

        if (n == 0) {
            /* Pipe was closed — supervisor exited */
            fprintf(stderr, "[log] pipe closed, exiting\n");
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;  /* signal interrupted read, retry */
            perror("read pipe");
            break;
        }
        if (n != sizeof(msg)) {
            fprintf(stderr, "[log] short read %zd bytes\n", n);
            continue;
        }

        /* Convert pipe_msg to log_record and append to file */
        struct log_record rec;
        rec.timestamp = msg.timestamp;
        rec.event     = msg.event;
        rec.soc       = msg.soc;
        rec.voltage   = msg.voltage;
        rec.current   = msg.current;
        rec.mode      = msg.mode;
        strncpy(rec.msg, msg.msg, sizeof(rec.msg) - 1);
        rec.msg[sizeof(rec.msg)-1] = '\0';

        /* Append the record at the current file position (end of file) */
        write(log_fd, &rec, sizeof(rec));

        /*
         * Now update record_count in the header.
         *
         * lseek(fd, offset, SEEK_SET) jumps to exact byte position from start.
         * offsetof(struct log_header, record_count) tells us exactly how many
         * bytes from the start of the struct to reach record_count.
         *
         * Without lseek, we'd have to read the whole file back and rewrite it!
         */
        hdr.record_count++;
        lseek(log_fd,
              offsetof(struct log_header, record_count),
              SEEK_SET);
        write(log_fd, &hdr.record_count, sizeof(hdr.record_count));

        /* Seek back to end of file so the next write() appends correctly */
        lseek(log_fd, 0, SEEK_END);

        fprintf(stderr, "[log] event #%u written\n", hdr.record_count);
    }

    close(log_fd);
    fprintf(stderr, "[log] done — %u events logged\n", hdr.record_count);
    return 0;
}
