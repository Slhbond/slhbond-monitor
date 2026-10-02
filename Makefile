# ---------------------------------------------------------------------------
# Slhbond-Monitor - pure C11 Linux system monitor
#
#   make            build ./build/slhbond-monitor
#   make debug      build with sanitizers and debug logging
#   make check      compile with the strictest warning set (used by CI)
#   make run        build and run in the foreground on :8090
#   make dist       build a deployable tarball
#   make install    install under $(PREFIX) plus /etc and systemd
#   make clean      remove build products
# ---------------------------------------------------------------------------

PROJECT   := slhbond-monitor
VERSION   := $(shell cat VERSION 2>/dev/null || echo 0.0.0-dev)

CC        ?= cc
PREFIX    ?= /opt/slhbond-monitor
BINDIR    ?= $(PREFIX)/bin
UNITDIR   ?= /etc/systemd/system
CONFDIR   ?= /etc
DESTDIR   ?=

SRCDIR    := src
WEBDIR    := web
DEPLOYDIR := deploy
OBJDIR    := build
BIN       := $(OBJDIR)/$(PROJECT)

SRCS      := $(wildcard $(SRCDIR)/*.c)
OBJS      := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS      := $(OBJS:.o=.d)

VERSION_DEFINE := -DSLH_VERSION_STRING='"$(VERSION)"'

# -MMD -MP emits header dependencies so a changed header rebuilds its users.
CPPFLAGS  += -D_GNU_SOURCE $(VERSION_DEFINE) -I$(SRCDIR)
CFLAGS    ?= -O2 -g
CFLAGS    += -std=gnu11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
             -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wwrite-strings
LDLIBS    += -lpthread

.PHONY: all debug check run dist install uninstall clean help

all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LD      $@"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@echo "  CC      $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

-include $(DEPS)

## Sanitizer build: catches the memory bugs that a daemon hides for months.
debug: CFLAGS := -O0 -g3 -std=gnu11 -Wall -Wextra -Wpedantic -Wshadow \
                 -fsanitize=address,undefined -fno-omit-frame-pointer
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(BIN)
	@echo "debug build ready: $(BIN)"

## Strictest warnings; fails the build on anything suspicious.
check: CFLAGS += -Werror -Wconversion -Wsign-conversion -Wcast-qual \
                 -Wundef -Wdouble-promotion -Wnull-dereference
check: clean all

run: $(BIN)
	./$(BIN) --foreground --docroot ./$(WEBDIR) --log-level debug

dist: $(BIN)
	@rm -rf dist && mkdir -p dist/$(PROJECT)-$(VERSION)
	@# deploy.sh 必须进包：它是目标机上唯一的安装入口。
	@cp -r $(SRCDIR) $(WEBDIR) $(DEPLOYDIR) docs Makefile VERSION README.md \
	      deploy.sh dist/$(PROJECT)-$(VERSION)/ 2>/dev/null || \
	 cp -r $(SRCDIR) $(WEBDIR) $(DEPLOYDIR) Makefile VERSION README.md \
	      deploy.sh dist/$(PROJECT)-$(VERSION)/
	@chmod +x dist/$(PROJECT)-$(VERSION)/deploy.sh
	@# 去掉可能带进来的 Windows 行尾，否则目标机上 bash 会报 bad interpreter
	@find dist/$(PROJECT)-$(VERSION) -type f \
	      \( -name '*.sh' -o -name '*.conf' -o -name '*.service' \) \
	      -exec sed -i 's/\r$$//' {} + 2>/dev/null || true
	@tar -C dist -czf dist/$(PROJECT)-$(VERSION).tar.gz $(PROJECT)-$(VERSION)
	@echo "wrote dist/$(PROJECT)-$(VERSION).tar.gz"

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(PROJECT)
	install -d $(DESTDIR)$(PREFIX)/$(WEBDIR)
	cp -r $(WEBDIR)/. $(DESTDIR)$(PREFIX)/$(WEBDIR)/
	install -m 0644 $(DEPLOYDIR)/slhbond-monitor.conf \
	       $(DESTDIR)$(CONFDIR)/slhbond-monitor.conf
	install -m 0644 $(DEPLOYDIR)/slhbond-monitor.service \
	       $(DESTDIR)$(UNITDIR)/slhbond-monitor.service
	@if [ ! -f $(DESTDIR)$(PREFIX)/update.json ]; then \
	   install -m 0644 $(DEPLOYDIR)/update.json.example \
	          $(DESTDIR)$(PREFIX)/update.json; fi
	@echo "installed. enable with: systemctl daemon-reload && systemctl enable --now $(PROJECT)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROJECT)
	rm -rf $(DESTDIR)$(PREFIX)/$(WEBDIR)
	rm -f $(DESTDIR)$(CONFDIR)/slhbond-monitor.conf
	rm -f $(DESTDIR)$(UNITDIR)/slhbond-monitor.service

clean:
	rm -rf $(OBJDIR) dist

help:
	@grep -B1 -E '^[a-zA-Z_-]+:' $(MAKEFILE_LIST) | grep -E '^##|^[a-zA-Z_-]+:' | head -40
