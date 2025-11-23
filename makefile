# ==========================
#  Simple nRF24 C Makefile
# ==========================

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Ilibs
LDFLAGS =

# Binaries we build
PROGRAMS = quick_mode fast_mode p2p_mode

# Common library objects
COMMON_OBJS = libs/nrf24.o libs/utils.o

# Per-program object lists
quick_mode_OBJS = quick_mode.o $(COMMON_OBJS)
fast_mode_OBJS  = fast_mode.o  $(COMMON_OBJS)
p2p_mode_OBJS   = p2p_mode.o   $(COMMON_OBJS)

.PHONY: all clean

all: $(PROGRAMS)

# -------- Pattern rules --------

# Generic rule to build any .o from .c (works in subdirs too)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# -------- Link rules for each binary --------

quick_mode: $(quick_mode_OBJS)
	$(CC) $(CFLAGS) -o $@ $(quick_mode_OBJS) $(LDFLAGS)

fast_mode: $(fast_mode_OBJS)
	$(CC) $(CFLAGS) -o $@ $(fast_mode_OBJS) $(LDFLAGS)

p2p_mode: $(p2p_mode_OBJS)
	$(CC) $(CFLAGS) -o $@ $(p2p_mode_OBJS) $(LDFLAGS)

# -------- Explicit deps for libs (optional but nice) --------

libs/nrf24.o: libs/nrf24.c libs/nrf24.h
libs/utils.o: libs/utils.c libs/utils.h

quick_mode.o: quick_mode.c libs/nrf24.h
fast_mode.o:  fast_mode.c  libs/nrf24.h
p2p_mode.o:   p2p_mode.c   libs/nrf24.h libs/utils.h

# -------- Cleanup --------

clean:
	rm -f $(PROGRAMS) *.o libs/*.o
