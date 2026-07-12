CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -pthread
LDLIBS   = -lncursesw -lm -pthread
PREFIX  ?= /usr/local

SRC := src/main.c src/topology.c src/counters.c src/perf.c src/trace.c src/ui.c
OBJ := $(SRC:.c=.o)

all: tlbanalyser

tlbanalyser: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c src/tlba.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: tlbanalyser
	install -D -m 0755 tlbanalyser $(DESTDIR)$(PREFIX)/bin/tlbanalyser

test/shootgen: test/shootgen.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

test: tlbanalyser test/shootgen
	./selftest.sh

clean:
	rm -f tlbanalyser test/shootgen $(OBJ)

.PHONY: all install clean test
