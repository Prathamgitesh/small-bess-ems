#ifndef IPC_CONFIG_H
#define IPC_CONFIG_H

#define SHM_NAME           "/bess_shm"
#define SEM_NAME           "/bess_sem"
#define MQ_NAME            "/bess_cmd"
#define FIFO_PATH          "/tmp/bess_telem"

#define PACK_CAPACITY_AH   100.0
#define PACK_VOLT_MAX      230.4
#define PACK_VOLT_MIN      179.2
#define PROT_OVERCURRENT_A 100.0

#define SIM_TIME_FACTOR    30        /* 1 real sec = 30 sim sec */
#define MQ_MAXMSG          10
#define MQ_MSGSIZE         64
#define WATCHDOG_S         5

#define MODE_IDLE          0
#define MODE_CHARGE        1
#define MODE_DISCHARGE     2
#define MODE_FAULT         3

#define FAULT_NONE         0
#define FAULT_OVERCURRENT  1

#define CMD_CHARGE         1
#define CMD_DISCHARGE      2
#define CMD_IDLE           3
#define CMD_TRIP           4
#define CMD_RESET          5

#define PRIO_TRIP          5
#define PRIO_CMD           1

#endif
