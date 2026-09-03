# Builds the C and C++ examples against an installed Astral.
PREFIX ?= $(HOME)/Astral

CFLAGS   += -O2 -I$(PREFIX)/include
CXXFLAGS += -O2 -std=c++17 -I$(PREFIX)/include
LDFLAGS  += -L$(PREFIX)/lib/astral/dynamic-libs -L$(PREFIX)/lib/astral/static-libs -L$(PREFIX)/lib
LDLIBS   += -lAstral -lz

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  LDLIBS += -lc++
else
  LDLIBS += -lstdc++
endif

all: c/decompile cpp/decompile

c/decompile: c/decompile.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

cpp/decompile: cpp/decompile.cpp
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f c/decompile cpp/decompile

.PHONY: all clean
