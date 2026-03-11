CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS = -lpthread -lrt -lm

PROGS = supervisor simulator scheduler protection logger dashboard

all: $(PROGS)

supervisor:  src/supervisor.c  include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

simulator:   src/simulator.c   include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

scheduler:   src/scheduler.c   include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) $< -o $@

protection:  src/protection.c  include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

logger:      src/logger.c      include/ipc_config.h include/bess_types.h
	$(CC) $(CFLAGS) $< -o $@

dashboard:   src/dashboard.c   include/ipc_config.h
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(PROGS) bess.log /tmp/bess_telem
	ipcrm -a 2>/dev/null || true

.PHONY: all clean
