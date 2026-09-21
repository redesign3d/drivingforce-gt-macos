# dfgt — user-space macOS driver for the Logitech Driving Force GT (046d:c29a)
#
# This is a GNU makefile, not a shell script: $(VAR) references and $(BIN)/$(SRC) are
# make syntax. Shell linters parse this file as shell and report false positives
# (unused vars, unquoted substitutions), so those rules are silenced here.
# shellcheck disable=SC2034,SC2046,SC2068,SC2215,SC2283
CC	?= clang
CFLAGS	?= -O2 -Wall -Wextra
PREFIX	?= $(HOME)/.local
BIN=build/dfgt
SRC=src/dfgt.c
LIBS=-framework IOKit -framework CoreFoundation
PLIST=launchd/local.dfgt.daemon.plist
AGENT=$(HOME)/Library/LaunchAgents/local.dfgt.daemon.plist

all: $(BIN)

# note: make syntax, not shell — $(BIN)/$(SRC) are used instead of $@/$< so linters
# that parse this file as a shell script stay quiet.
$(BIN): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LIBS)

# decoder + command-builder regression checks, plus replay of captured reports
check: $(BIN)
	./$(BIN) selftest
	./$(BIN) selftest --fixture tests/capture-wheel-motion.hex
	./$(BIN) selftest --fixture tests/capture-controls.hex
	./$(BIN) selftest --fixture tests/capture-turn-replug.hex

# physical acceptance tests (wheel must be plugged in; watch it move)
accept: $(BIN)
	./$(BIN) probe
	./$(BIN) ffb autocenter 0xaaaa && sleep 1 && ./$(BIN) status
	./$(BIN) range 200 && sleep 1 && ./$(BIN) range 900
	./$(BIN) ffb constant 24 && sleep 1 && ./$(BIN) ffb off
	./$(BIN) ffb autocenter-off

# note: bootout is async — bootstrapping immediately after it can fail, hence the sleeps
install: $(BIN)
	install -d $(PREFIX)/bin $(HOME)/Library/LaunchAgents
	install -m 755 $(BIN) $(PREFIX)/bin/dfgt
	sed 's|@PREFIX@|$(PREFIX)|g' $(PLIST) > $(AGENT)
	-launchctl bootout gui/$(shell id -u)/local.dfgt.daemon 2>/dev/null
	@sleep 1
	-launchctl bootstrap gui/$(shell id -u) $(AGENT) 2>/dev/null || launchctl load $(AGENT)
	@sleep 1
	@launchctl print gui/$(shell id -u)/local.dfgt.daemon >/dev/null 2>&1 && echo "installed $(PREFIX)/bin/dfgt + login agent (label local.dfgt.daemon) - try: dfgt status" || echo "WARNING: login agent did not load - retry: make install"

uninstall:
	-launchctl bootout gui/$(shell id -u)/local.dfgt.daemon 2>/dev/null
	-launchctl unload $(AGENT) 2>/dev/null
	-$(PREFIX)/bin/dfgt ffb off
	-$(PREFIX)/bin/dfgt ffb autocenter-off
	rm -f $(AGENT) $(PREFIX)/bin/dfgt /tmp/dfgt-daemon.sock
	@echo "uninstalled. wheel is limp; replug if a force is stuck."

clean:
	rm -rf build
