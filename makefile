CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Ilibs
LDFLAGS = 

# Source files
SRCS    = quick_mode.c libs/nrf24.c
OBJS    = $(SRCS:.c=.o)

all: quick_mode

quick_mode: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Generic rule: builds both quick_mode.o and libs/nrf24.o
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f quick_mode $(OBJS)
