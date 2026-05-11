CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -lm

all: workload_sim

workload_sim: workload_sim.c
	$(CC) $(CFLAGS) -o $@ workload_sim.c

clean:
	rm -f workload_sim
