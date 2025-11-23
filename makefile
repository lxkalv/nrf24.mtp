CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Ilibs
LDFLAGS = 

# Source files
LIBS    = libs/nrf24.c

SRCS_FM = fast_mode.c   $(LIBS)
SRCS_QM = quick_mode.c  $(LIBS)
SRCS_SC = simple_chat.c $(LIBS)

OBJS_FM = $(SRCS_FM:.c=.o)
OBJS_QM = $(SRCS_QM:.c=.o)
OBJS_SC = $(SRCS_SC:.c=.o)

all: quick_mode

fast_mode: $(OBJS_FM)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

simple_chat: $(OBJS_SC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

quick_mode: $(OBJS_QM)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Generic rule: builds both quick_mode.o and libs/nrf24.o
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f quick_mode fast_mode simple_chat $(OBJS_FM) $(OBJS_QM) $(OBJS_SC)
