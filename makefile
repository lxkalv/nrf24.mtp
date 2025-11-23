# ==========================
#  Simple nRF24 C Makefile
# ==========================

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Ilibs
LDFLAGS =

# Where to put the executables
BINDIR  = bin

# Program names (without path)
PROGS   = quick_mode fast_mode p2p_mode

# Executables with full path
BIN_PROGS = $(addprefix $(BINDIR)/,$(PROGS))

# Common library objects
COMMON_OBJS = libs/nrf24.o libs/utils.o

# Per-program object lists
quick_mode_OBJS = quick_mode.o $(COMMON_OBJS)
fast_mode_OBJS  = fast_mode.o  $(COMMON_OBJS)
p2p_mode_OBJS   = p2p_mode.o   $(COMMON_OBJS)

.PHONY: all clean

# Default target: build all executables into bin/
all: $(BINDIR) $(BIN_PROGS)

# Ensure bin/ exists
$(BINDIR):
	@mkdir -p $(BINDIR)

# ---------- Pattern rule for .o ----------

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- Link rules ----------

$(BINDIR)/quick_mode: $(quick_mode_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(quick_mode_OBJS) $(LDFLAGS)

$(BINDIR)/fast_mode: $(fast_mode_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(fast_mode_OBJS) $(LDFLAGS)

$(BINDIR)/p2p_mode: $(p2p_mode_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(p2p_mode_OBJS) $(LDFLAGS)

# ---------- Explicit deps (nice but optional) ----------

libs/nrf24.o: libs/nrf24.c libs/nrf24.h
libs/utils.o: libs/utils.c libs/utils.h

quick_mode.o: quick_mode.c libs/nrf24.h
fast_mode.o:  fast_mode.c  libs/nrf24.h
p2p_mode.o:   p2p_mode.c   libs/nrf24.h libs/utils.h

# ---------- Cleanup ----------

clean:
	rm -f $(BIN_PROGS) *.o libs/*.o
	@rmdir $(BINDIR) 2>/dev/null || true
