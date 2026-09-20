# Standalone - not part of the Write build. Built once by
# tools/install-agent-display.sh and used only by tools/agent-display.sh.
#
# The protocol XML is vendored in protocol/ (taken from wlr-protocols via
# wlrctl) because Arch's wayland-protocols package does not ship the wlr-*
# extensions, so there is no system path to point wayland-scanner at.

CFLAGS ?= -O2 -Wall -Wextra
WAYLAND_FLAGS := $(shell pkg-config --cflags --libs wayland-client)
SCANNER := wayland-scanner

PROTO := protocol/wlr-virtual-pointer-unstable-v1.xml
GEN_H := wlr-virtual-pointer-unstable-v1-client-protocol.h
GEN_C := wlr-virtual-pointer-unstable-v1-protocol.c

all: agent-pointer

$(GEN_H): $(PROTO)
	$(SCANNER) client-header $< $@

$(GEN_C): $(PROTO)
	$(SCANNER) private-code $< $@

agent-pointer: agent-pointer.c $(GEN_C) $(GEN_H)
	$(CC) $(CFLAGS) -I. -o $@ agent-pointer.c $(GEN_C) $(WAYLAND_FLAGS)

clean:
	rm -f agent-pointer $(GEN_H) $(GEN_C)

.PHONY: all clean
