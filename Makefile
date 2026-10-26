# Makefile for du_lazy — a statx-based disk usage analyzer.
#
# This Makefile follows the GNU Makefile conventions:
# https://www.gnu.org/prep/standards/html_node/Makefile-Conventions.html

# Package metadata
PACKAGE = fue
VERSION = $(shell git describe --tags 2>/dev/null)

# Programs and utilities
INSTALL         = install
INSTALL_PROGRAM = $(INSTALL)
INSTALL_DATA    = $(INSTALL) -m 644
MKDIR_P     = mkdir -p
RM          = rm -f

# Build flags.  Users may override these on the command line, e.g.
#   make CFLAGS='-O3 -march=native'
CC          = gcc
CPPFLAGS    =
CFLAGS      = -O2 -g -std=c11 -Wall -Wextra
LDFLAGS     =
LDLIBS      =

# Flags the build requires regardless of the user's CFLAGS.
PTHREAD_FLAGS = -pthread

# The interactive browser (--tui) needs ncursesw.  Build with WITH_TUI=no to
# leave it out on systems without the ncurses headers (run "make clean" first
# when switching).
WITH_TUI       = yes
NCURSES_CFLAGS = $(shell pkg-config --cflags ncursesw 2>/dev/null)
NCURSES_LIBS   = $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)

# Installation directories (GNU standard).  These are overridable, and
# DESTDIR is honored for staged installs (e.g. packaging).
prefix      = /usr/local
exec_prefix = $(prefix)
bindir      = $(exec_prefix)/bin
datarootdir = $(prefix)/share
docdir      = $(datarootdir)/doc/$(PACKAGE)
mandir      = $(datarootdir)/man
man1dir     = $(mandir)/man1

# Project layout
srcdir      = src
SRCS        = $(srcdir)/main.c
HEADERS     = $(srcdir)/fue.h
PROGRAM     = $(PACKAGE)

ifeq ($(WITH_TUI),yes)
SRCS       += $(srcdir)/tui.c
TUI_LIBS    = $(NCURSES_LIBS)
else
TUI_DEFS    = -DFUE_NO_TUI
endif
OBJS        = $(SRCS:.c=.o)

# Default target.
.PHONY: all
all: $(PROGRAM)

# Link the executable.
$(PROGRAM): $(OBJS)
	$(CC) $(PTHREAD_FLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(TUI_LIBS) $(LDLIBS)

# Version string baked into the binary at compile time.
VERSION_DEF = -DFUE_VERSION='"$(VERSION)"'

# Compile objects.  Pattern rule mirrors the built-in GNU rule but is
# spelled out for clarity.
$(srcdir)/%.o: $(srcdir)/%.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(VERSION_DEF) $(TUI_DEFS) $(EXTRA_CPPFLAGS) $(PTHREAD_FLAGS) $(CFLAGS) -c -o $@ $<

$(srcdir)/tui.o: EXTRA_CPPFLAGS = $(NCURSES_CFLAGS)

# Install / uninstall.
.PHONY: install
install: all
	$(MKDIR_P) $(DESTDIR)$(bindir)
	$(INSTALL_PROGRAM) $(PROGRAM) $(DESTDIR)$(bindir)/$(PROGRAM)

.PHONY: install-strip
install-strip:
	$(MAKE) INSTALL_PROGRAM='$(INSTALL_PROGRAM) -s' install

.PHONY: uninstall
uninstall:
	$(RM) $(DESTDIR)$(bindir)/$(PROGRAM)

# Cleanup targets.
.PHONY: clean
clean:
	$(RM) $(srcdir)/*.o $(PROGRAM)

.PHONY: distclean
distclean: clean

.PHONY: mostlyclean
mostlyclean: clean

.PHONY: maintainer-clean
maintainer-clean: distclean

# Run a quick smoke test against the current directory.
.PHONY: check
check: all
	./$(PROGRAM) --no-cache .

# Convenience target.
.PHONY: help
help:
	@echo 'Targets:'
	@echo '  all            Build $(PROGRAM) (default)'
	@echo '  install        Install to $$(DESTDIR)$$(bindir)'
	@echo '  install-strip  Install, stripping symbols'
	@echo '  uninstall      Remove the installed program'
	@echo '  check          Build and run a smoke test'
	@echo '  clean          Remove build artifacts'
	@echo '  distclean      Alias for clean'
	@echo
	@echo 'Common overrides: CC, CFLAGS, CPPFLAGS, LDFLAGS, prefix, DESTDIR'
