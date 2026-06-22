# Makefile for du_lazy — a statx-based disk usage analyzer.
#
# This Makefile follows the GNU Makefile conventions:
# https://www.gnu.org/prep/standards/html_node/Makefile-Conventions.html

# Package metadata
PACKAGE     = du_lazy
VERSION     = 1.0.0

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
OBJS        = $(SRCS:.c=.o)
PROGRAM     = $(PACKAGE)

# Default target.
.PHONY: all
all: $(PROGRAM)

# Link the executable.
$(PROGRAM): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# Compile objects.  Pattern rule mirrors the built-in GNU rule but is
# spelled out for clarity.
$(srcdir)/%.o: $(srcdir)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

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
	$(RM) $(OBJS) $(PROGRAM)

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
