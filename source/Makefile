#!/usr/bin/make -f
# Makefile for alo.lv2 #
# ----------------------- #
#

include Makefile.mk

# --------------------------------------------------------------

PREFIX  ?= /usr/local
DESTDIR ?=

# --------------------------------------------------------------
# Default target is to build all plugins

all: build

# --------------------------------------------------------------
# alo build rules

build: alo.lv2/alo$(LIB_EXT) alo.lv2/manifest.ttl

alo.lv2/alo$(LIB_EXT): alo.c
	$(CXX) $^ $(BUILD_CXX_FLAGS) $(LINK_FLAGS) -lm $(SHARED) -o $@

alo.lv2/manifest.ttl: alo.lv2/manifest.ttl.in
	sed -e "s|@LIB_EXT@|$(LIB_EXT)|" $< > $@

# --------------------------------------------------------------

clean:
	rm -f alo.lv2/alo$(LIB_EXT) alo.lv2/manifest.ttl

# --------------------------------------------------------------

install: build
	install -d $(DESTDIR)$(PREFIX)/lib/lv2/alo.lv2
	install -d $(DESTDIR)$(PREFIX)/lib/lv2/alo.lv2/modgui

	install -m 644 alo.lv2/*.so  $(DESTDIR)$(PREFIX)/lib/lv2/alo.lv2/
	install -m 644 alo.lv2/*.ttl $(DESTDIR)$(PREFIX)/lib/lv2/alo.lv2/
	install -m 644 alo.lv2/modgui/* $(DESTDIR)$(PREFIX)/lib/lv2/alo.lv2/modgui

# --------------------------------------------------------------
