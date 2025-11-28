CC      = gcc
CFLAGS  = -Wall -Wextra -O3 -std=c11
LDFLAGS = -lz

OBJS_COMMON = nrf24.o logger.o
OBJS_P3P    = p3p_mode.o $(OBJS_COMMON)
all: p3p_mode

p3p_mode: $(OBJS_P3P)
	$(CC) $(CFLAGS) -o $@ $(OBJS_P3P) $(LDFLAGS)

p3p_mode.o: p3p_mode.c nrf24.h logger.h
	$(CC) $(CFLAGS) -c p3p_mode.c

nrf24.o: nrf24.c nrf24.h
	$(CC) $(CFLAGS) -c nrf24.c

logger.o: logger.c logger.h
	$(CC) $(CFLAGS) -c logger.c

clean:
	rm -f *.o p3p_mode
