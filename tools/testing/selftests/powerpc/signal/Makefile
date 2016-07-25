TEST_PROGS := signal signal_tm

all: $(TEST_PROGS)

$(TEST_PROGS): ../harness.c ../utils.c signal.S

signal_tm: CFLAGS += -mhtm

include ../../lib.mk

clean:
	rm -f $(TEST_PROGS) *.o
