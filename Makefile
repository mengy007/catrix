# Makefile for your Matrix C program
# Usage:
#   make            # build (default: release)
#   make debug      # build with debug flags
#   make run        # run ./build/matrix
#   make install    # install to $(PREFIX)/bin (default: /usr/local)
#   make uninstall  # remove installed binary
#   make clean      # remove build artifacts
#
# Optional:
#   make install PREFIX=/opt/homebrew   # e.g., on Apple Silicon Homebrew
#   make CFLAGS_EXTRA='-march=native'   # add extra flags
#   make LDLIBS='-lrt'                  # if your Linux needs -lrt for clock_gettime

# ---- project ----
APP      := catrix
SRC      := catrix.c
BUILD    := build
BIN      := $(BUILD)/$(APP)

# ---- toolchain & flags ----
CC       := cc
# feature test macro for POSIX APIs (clock_gettime, nanosleep, etc.)
BASE_DEFS := -D_DARWIN_C_SOURCE -D_POSIX_C_SOURCE=200809L
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wmissing-prototypes
OPT_REL  := -O3
OPT_DBG  := -O0 -g3
STD      := -std=c11
CFLAGS_COMMON := $(STD) $(BASE_DEFS) $(WARN) $(CFLAGS_EXTRA)

# Default build is release; 'make debug' will override
CFLAGS   := $(CFLAGS_COMMON) $(OPT_REL)
LDFLAGS  :=
LDLIBS   := $(LDLIBS)   # allow override from CLI

# On some older Linux toolchains, you may need -lrt for clock_gettime:
#   make LDLIBS='-lrt'
#
# If you ever switch to ncurses, you can do:
#   make LDLIBS="$(shell pkg-config --libs ncurses 2>/dev/null || echo -lncurses)"

# ---- install paths ----
PREFIX   ?= /usr/local
DESTDIR  ?=
BINDIR   := $(DESTDIR)$(PREFIX)/bin

# ---- rules ----
.PHONY: all debug run clean install uninstall

all: $(BIN)

debug: CFLAGS := $(CFLAGS_COMMON) $(OPT_DBG)
debug: $(BIN)

$(BUILD):
	@mkdir -p $(BUILD)

$(BIN): $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

run: $(BIN)
	@$(BIN)

clean:
	@rm -rf $(BUILD)

install: $(BIN)
	@mkdir -p "$(BINDIR)"
	@install -m 0755 $(BIN) "$(BINDIR)/$(APP)"
	@echo "Installed $(APP) to $(BINDIR)"

uninstall:
	@rm -f "$(BINDIR)/$(APP)"
	@echo "Removed $(BINDIR)/$(APP)"
