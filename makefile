# ==========================
#  nRF24 C Project Makefile
# ==========================

CC      = gcc
SRCDIR  = src
LIBDIR  = libs
BINDIR  = bin

CFLAGS  = -Wall -Wextra -O2 -I. -I$(LIBDIR)
LDFLAGS =

# Programs (names only)
PROGS     = quick_mode fast_mode p2p_mode
BIN_PROGS = $(addprefix $(BINDIR)/,$(PROGS))

# Common library objects
COMMON_OBJS   = $(LIBDIR)/nrf24.o $(LIBDIR)/utils.o
quick_mode_OBJS = $(SRCDIR)/quick_mode.o $(COMMON_OBJS)
fast_mode_OBJS  = $(SRCDIR)/fast_mode.o  $(COMMON_OBJS)
p2p_mode_OBJS   = $(SRCDIR)/p2p_mode.o   $(COMMON_OBJS)

.PHONY: all clean

# ---------- Default target ----------

all: $(BINDIR) $(BIN_PROGS)

# Ensure bin/ exists
$(BINDIR):
	@mkdir -p $(BINDIR)

# ---------- Compilation rules ----------

# Program sources in src/
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Library sources in libs/
$(LIBDIR)/%.o: $(LIBDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- Link rules ----------

$(BINDIR)/quick_mode: $(quick_mode_OBJS)
	$(CC) $(CFLAGS) -o $@ $(quick_mode_OBJS) $(LDFLAGS)

$(BINDIR)/fast_mode: $(fast_mode_OBJS)
	$(CC) $(CFLAGS) -o $@ $(fast_mode_OBJS) $(LDFLAGS)

$(BINDIR)/p2p_mode: $(p2p_mode_OBJS)
	$(CC) $(CFLAGS) -o $@ $(p2p_mode_OBJS) $(LDFLAGS)

# ---------- Explicit dependencies (nice to have) ----------

$(SRCDIR)/quick_mode.o: $(SRCDIR)/quick_mode.c $(LIBDIR)/nrf24.h
$(SRCDIR)/fast_mode.o:  $(SRCDIR)/fast_mode.c  $(LIBDIR)/nrf24.h
$(SRCDIR)/p2p_mode.o:   $(SRCDIR)/p2p_mode.c   $(LIBDIR)/nrf24.h $(LIBDIR)/utils.h

$(LIBDIR)/nrf24.o: $(LIBDIR)/nrf24.c $(LIBDIR)/nrf24.h
$(LIBDIR)/utils.o: $(LIBDIR)/utils.c $(LIBDIR)/utils.h

# ---------- Cleanup ----------

clean:
	rm -f $(BIN_PROGS) $(SRCDIR)/*.o $(LIBDIR)/*.o
	@rmdir $(BINDIR) 2>/dev/null || true
