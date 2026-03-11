# Makefile for small-bess-ems
#
# Usage:
#   make        — build all programs
#   make clean  — remove all built files
#   ./supervisor — run the whole simulation

CC     = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude
LFLAGS = -lpthread -lrt -lm

# All programs to build
PROGRAMS = supervisor simulator scheduler protection logger dashboard

all: $(PROGRAMS)

# Each program links its own .c file + needs headers
supervisor:  src/supervisor.c  include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) src/supervisor.c  -o supervisor  $(LFLAGS)

simulator:   src/simulator.c   include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) src/simulator.c   -o simulator   $(LFLAGS)

scheduler:   src/scheduler.c   include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) src/scheduler.c   -o scheduler

protection:  src/protection.c  include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) src/protection.c  -o protection  $(LFLAGS)

logger:      src/logger.c      include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) src/logger.c      -o logger

dashboard:   src/dashboard.c   include/ipc_config.h
	$(CC) $(CFLAGS) src/dashboard.c   -o dashboard

clean:
	rm -f $(PROGRAMS) bess.log
	rm -f /tmp/bess_telem
	# Clean up leftover IPC objects (if simulation crashed)
	# These live in /dev/shm on Linux
	rm -f /dev/shm/bess_shm
	rm -f /dev/mqueue/bess_cmd

.PHONY: all clean
