CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -L:libs/
LDFLAGS = 

all: quick_mode

quick_mode: quick_mode.o nrf24.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c nrf24.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f quick_mode *.o
