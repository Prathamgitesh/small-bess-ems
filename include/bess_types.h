#ifndef BESS_TYPES_H
#define BESS_TYPES_H

/*
 * bess_types.h
 * ------------
 * Defines the data structures (structs) shared between all processes.
 *
 * NOTE: We are NOT using typedef here on purpose.
 * To use these structs, write:  struct bess_state  (not bess_state_t)
 */

#include <time.h>     /* for time_t  */
#include <stdint.h>   /* for uint64_t */


/*
 * struct bess_state
 * -----------------
 * This is the "live status" of the battery.
 * The simulator writes this into shared memory every second.
 * The protection process and dashboard read it.
 */
struct bess_state {
    double   soc;          /* State of Charge: 0.0 = empty, 1.0 = full      */
    double   voltage;      /* Pack voltage in Volts (calculated from soc)   */
    double   current;      /* Current in Amps: positive=charging, negative=discharging */
    int      mode;         /* What mode is the battery in? (MODE_* values)  */
    int      fault_code;   /* What fault happened? (FAULT_* values)         */
    uint64_t tick;         /* Counter: goes up by 1 every time we write shm */
    time_t   last_update;  /* Unix timestamp of last write                  */
};


/*
 * struct bess_cmd
 * ---------------
 * A command message sent over the message queue.
 * Scheduler sends these to tell simulator what to do.
 * Protection sends CMD_TRIP when overcurrent is detected.
 */
struct bess_cmd {
    int    cmd;        /* Which command? (CMD_* values)                     */
    double param;      /* Extra info: current in Amps, or fault code        */
    char   label[48];  /* Human-readable description (for logs/display)     */
};


/*
 * struct pipe_msg
 * ---------------
 * A log event sent through the anonymous pipe from supervisor to logger.
 * Every time something important happens, supervisor writes one of these.
 */
struct pipe_msg {
    time_t timestamp;  /* When did this happen?                             */
    int    event;      /* Event type: 0=mode change, 1=fault, 2=reset      */
    double soc;        /* Battery SoC at time of event                     */
    double voltage;    /* Battery voltage at time of event                 */
    double current;    /* Battery current at time of event                 */
    int    mode;       /* Battery mode at time of event                    */
    char   msg[64];    /* Human-readable description                       */
};


/*
 * struct log_record
 * -----------------
 * One record written to the binary log file by logger.c
 * Each record is a fixed size so lseek() can jump around easily.
 */
struct log_record {
    time_t timestamp;
    int    event;
    double soc;
    double voltage;
    double current;
    int    mode;
    char   msg[64];
};


/*
 * struct log_header
 * -----------------
 * The first thing written in the binary log file.
 * record_count is updated every time we add a new record (using lseek).
 */
struct log_header {
    char     magic[4];       /* Always "BESS" — helps identify the file   */
    uint32_t record_count;   /* How many log records are in this file?    */
    time_t   session_start;  /* When did this session start?              */
};

#endif  /* BESS_TYPES_H */
