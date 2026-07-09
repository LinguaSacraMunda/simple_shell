.PHONY: all clean

all: shell

CC = gcc
CFLAGS = -g -Wall -Wextra -O3 -lreadline
SHELL= /bin/bash

shell: shell.o
	$(CC) $(CFLAGS) $^ -o $@

%.s: %.c
	$(CC) $(CFLAGS) -S -fverbose-asm $<

%.o: %.c
	$(CC) $(CFLAGS) -c $<

%.i: %.c
	gcc -Wall -E $< | indent -kr > $@

clean:
	rm -f *.o shell
