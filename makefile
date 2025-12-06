# ==========================
#  nRF24 C Project Makefile
# ==========================

CC      = gcc
SRCDIR  = src
LIBDIR  = libs
BINDIR  = bin

CFLAGS  = -Wall -Wextra -O2 -I. -I$(LIBDIR)
LDFLAGS = -lz

# Programs (names only)
# Added robust_mode_reset here
PROGS     = quick_mode fast_mode p2p_mode p3p_mode robust_mode robust_mode_reset
BIN_PROGS = $(addprefix $(BINDIR)/,$(PROGS))

# Library objects
LIB_OBJS = \
    $(LIBDIR)/nrf24.o \
    $(LIBDIR)/logger.o \
    $(LIBDIR)/app_layer.o \
    $(LIBDIR)/presentation_layer.o \
    $(LIBDIR)/transport_layer.o \
    $(LIBDIR)/link_layer.o

.DEFAULT_GOAL := help

# ---------- Default / convenience targets ----------

.PHONY: help all quick fast p2p p3p robust robust_reset clean

help:
	@echo "Available targets:"
	@echo "  make quick         -> build bin/quick_mode"
	@echo "  make fast          -> build bin/fast_mode"
	@echo "  make p2p           -> build bin/p2p_mode"
	@echo "  make p3p           -> build bin/p3p_mode"
	@echo "  make robust        -> build bin/robust_mode"
	@echo "  make robust_reset  -> build bin/robust_mode_reset"
	@echo "  make all           -> build every mode"
	@echo "  make clean         -> remove binaries and objects"

all: $(BIN_PROGS)

quick: $(BINDIR)/quick_mode

fast: $(BINDIR)/fast_mode

p2p: $(BINDIR)/p2p_mode

p3p: $(BINDIR)/p3p_mode

robust: $(BINDIR)/robust_mode

robust_reset: $(BINDIR)/robust_mode_reset

# ---------- Generic compilation rules ----------

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIBDIR)/%.o: $(LIBDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---------- Link targets ----------

$(BINDIR)/quick_mode: $(SRCDIR)/quick_mode.o $(LIBDIR)/nrf24.o $(LIBDIR)/utils.o $(LIBDIR)/logger.o
	mkdir -p $(BINDIR)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BINDIR)/fast_mode: $(SRCDIR)/fast_mode.o $(LIBDIR)/nrf24.o $(LIBDIR)/utils.o $(LIBDIR)/logger.o
	mkdir -p $(BINDIR)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BINDIR)/p2p_mode: $(SRCDIR)/p2p_mode.o $(LIBDIR)/nrf24.o $(LIBDIR)/utils.o $(LIBDIR)/logger.o
	mkdir -p $(BINDIR)
	$(CC) -o $@ $^ $(LDFLAGS)

# New p3p_mode: uses all layered libs
$(BINDIR)/p3p_mode: $(SRCDIR)/p3p_mode.o $(LIB_OBJS)
	mkdir -p $(BINDIR)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BINDIR)/robust_mode: $(SRCDIR)/robust_mode.o $(LIBDIR)/nrf24.o $(LIBDIR)/logger.o $(LIBDIR)/app_layer.o
	mkdir -p $(BINDIR)
	$(CC) -o $@ $^ $(LDFLAGS)

# Added robust_mode_reset linkage
# CORRECCIÓN AQUÍ: Se ha añadido $(LIBDIR)/app_layer.o a la lista de dependencias
$(BINDIR)/robust_mode_reset: $(SRCDIR)/robust_mode_reset.o $(LIBDIR)/nrf24.o $(LIBDIR)/utils.o $(LIBDIR)/logger.o $(LIBDIR)/app_layer.o
	mkdir -p $(BINDIR)
	$(CC) -o $@ $^ $(LDFLAGS)

# ---------- Explicit dependencies (nice to have) ----------

$(SRCDIR)/quick_mode.o: $(SRCDIR)/quick_mode.c $(LIBDIR)/nrf24.h
$(SRCDIR)/fast_mode.o:  $(SRCDIR)/fast_mode.c  $(LIBDIR)/nrf24.h
$(SRCDIR)/p2p_mode.o:   $(SRCDIR)/p2p_mode.c   $(LIBDIR)/nrf24.h
$(SRCDIR)/p3p_mode.o:   $(SRCDIR)/p3p_mode.c   \
                        $(LIBDIR)/logger.h \
                        $(LIBDIR)/app_layer.h \
                        $(LIBDIR)/presentation_layer.h \
                        $(LIBDIR)/transport_layer.h \
                        $(LIBDIR)/link_layer.h \
                        $(LIBDIR)/nrf24.h
$(SRCDIR)/robust_mode.o: $(SRCDIR)/robust_mode.c $(LIBDIR)/nrf24.h $(LIBDIR)/logger.h $(LIBDIR)/app_layer.h

# Added robust_mode_reset dependencies
# CORRECCIÓN AQUÍ: Se añade app_layer.h
$(SRCDIR)/robust_mode_reset.o: $(SRCDIR)/robust_mode_reset.c $(LIBDIR)/nrf24.h $(LIBDIR)/logger.h $(LIBDIR)/app_layer.h

$(LIBDIR)/nrf24.o:            $(LIBDIR)/nrf24.c            $(LIBDIR)/nrf24.h $(LIBDIR)/logger.h
$(LIBDIR)/utils.o:            $(LIBDIR)/utils.c            $(LIBDIR)/utils.h
$(LIBDIR)/logger.o:           $(LIBDIR)/logger.c           $(LIBDIR)/logger.h
$(LIBDIR)/app_layer.o:        $(LIBDIR)/app_layer.c        $(LIBDIR)/app_layer.h
$(LIBDIR)/presentation_layer.o: $(LIBDIR)/presentation_layer.c $(LIBDIR)/presentation_layer.h
$(LIBDIR)/transport_layer.o:  $(LIBDIR)/transport_layer.c  $(LIBDIR)/transport_layer.h
$(LIBDIR)/link_layer.o:       $(LIBDIR)/link_layer.c       $(LIBDIR)/link_layer.h $(LIBDIR)/nrf24.h

# ---------- Cleanup ----------

clean:
	rm -f $(BIN_PROGS) $(SRCDIR)/*.o $(LIBDIR)/*.o
	@rmdir $(BINDIR) 2>/dev/null || true