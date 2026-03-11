#ifndef IPC_CONFIG_H
#define IPC_CONFIG_H

/*
 * ipc_config.h
 * ------------
 * This file holds all the "magic numbers" used across the project.
 * Instead of writing 100.0 everywhere, we give it a name here.
 * Every .c file includes this so they all share the same values.
 */

/* --- Names for IPC objects ---
 * These are like filenames for shared memory, semaphores, etc.
 * They must start with "/" on Linux.
 */
#define SHM_NAME        "/bess_shm"    /* shared memory name   */
#define SEM_NAME        "/bess_sem"    /* semaphore name       */
#define MQ_NAME         "/bess_cmd"    /* message queue name   */
#define FIFO_PATH       "/tmp/bess_telem"  /* named pipe path  */

/* --- Battery pack numbers --- */
#define PACK_CAPACITY_AH   100.0   /* battery can hold 100 Ah of charge    */
#define PACK_VOLT_MAX      230.4   /* highest voltage when fully charged    */
#define PACK_VOLT_MIN      179.2   /* lowest voltage when fully drained     */

/* --- Protection --- */
#define PROT_OVERCURRENT_A 100.0   /* if current exceeds 100A → TRIP        */

/* --- Timing ---
 * We speed up simulated time so you don't have to wait hours.
 * 1 real second = 30 simulated seconds.
 */
#define SIM_TIME_FACTOR    30

/* --- Message queue limits --- */
#define MQ_MAXMSG          10    /* max 10 messages waiting in the queue   */
#define MQ_MSGSIZE         64    /* each message is at most 64 bytes       */

/* --- Watchdog --- */
#define WATCHDOG_S         5    /* supervisor restarts child if stuck 5s  */

/* --- Operating modes (what is the battery doing right now?) --- */
#define MODE_IDLE          0    /* battery is doing nothing               */
#define MODE_CHARGE        1    /* battery is charging                    */
#define MODE_DISCHARGE     2    /* battery is discharging                 */
#define MODE_FAULT         3    /* something went wrong, stopped          */

/* --- Fault codes (what went wrong?) --- */
#define FAULT_NONE         0    /* no fault                               */
#define FAULT_OVERCURRENT  1    /* too much current flowing               */

/* --- Commands (what can we ask the simulator to do?) --- */
#define CMD_CHARGE         1    /* start charging at X amps               */
#define CMD_DISCHARGE      2    /* start discharging at X amps            */
#define CMD_IDLE           3    /* stop everything                        */
#define CMD_TRIP           4    /* emergency stop (sent by protection)    */
#define CMD_RESET          5    /* clear the fault, go back to idle       */

/* --- Message queue priorities ---
 * Higher number = delivered first.
 * TRIP is priority 5 so it jumps ahead of normal commands (priority 1).
 */
#define PRIO_TRIP          5
#define PRIO_CMD           1

#endif  /* IPC_CONFIG_H */
