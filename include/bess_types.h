#ifndef BESS_TYPES_H
#define BESS_TYPES_H

#include <time.h>
#include <stdint.h>

/* shared memory — simulator writes, protection+dashboard read */
typedef struct {
    double   soc;           /* 0.0 – 1.0                       */
    double   voltage;       /* OCV(soc), no IR drop            */
    double   current;       /* +ve = charge, -ve = discharge   */
    int      mode;
    int      fault_code;
    uint64_t tick;
    time_t   last_update;
} bess_state_t;

/* message queue payload */
typedef struct {
    int    cmd;
    double param;           /* current (A) or fault code       */
    char   label[48];
} bess_cmd_t;

/* pipe message — supervisor writes, logger reads */
typedef struct {
    time_t timestamp;
    int    event;
    double soc;
    double voltage;
    double current;
    int    mode;
    char   msg[64];
} pipe_msg_t;

/* binary log record written by logger */
typedef struct {
    time_t  timestamp;
    int     event;
    double  soc;
    double  voltage;
    double  current;
    int     mode;
    char    msg[64];
} log_record_t;

/* binary log file header — record_count updated via lseek */
typedef struct {
    char     magic[4];       /* "BESS"                         */
    uint32_t record_count;
    time_t   session_start;
} log_header_t;

#endif
