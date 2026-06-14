TARGET_NAME = altair8800

ifeq ($(platform),)
  platform = unix
  ifeq ($(shell uname -s),Darwin)
    platform = osx
  endif
  ifeq ($(OS),Windows_NT)
    platform = win
  endif
endif

# ── Linux / BSD ──────────────────────────────────────────────
ifeq ($(platform), unix)
  TARGET := $(TARGET_NAME)_libretro.so
  CC     ?= gcc
  CFLAGS  = -fPIC -O2 -Wall -std=c99
  LFLAGS  = -shared -fPIC

# ── macOS (fat binary: x86_64 + arm64) ───────────────────────
else ifeq ($(platform), osx)
  TARGET := $(TARGET_NAME)_libretro.dylib
  CC      = cc
  CFLAGS  = -fPIC -O2 -Wall -std=c99 -arch x86_64 -arch arm64
  LFLAGS  = -dynamiclib -fPIC -arch x86_64 -arch arm64

# ── Windows (mingw cross) ─────────────────────────────────────
else ifeq ($(platform), win)
  TARGET := $(TARGET_NAME)_libretro.dll
  CC     ?= x86_64-w64-mingw32-gcc
  # -U_FORTIFY_SOURCE must come AFTER any -D, so put it last
  CFLAGS  = -O2 -Wall -std=c99 -U_FORTIFY_SOURCE
  LFLAGS  = -shared -static-libgcc

endif

OBJECTS = libretro.o i8080.o

.PHONY: all clean

all: $(TARGET)

libretro.o: libretro.c
	$(CC) $(CFLAGS) -c -o $@ $

i8080.o: i8080.c
	$(CC) $(CFLAGS) -c -o $@ $

$(TARGET): $(OBJECTS)
	$(CC) $(LFLAGS) -o $@ $(OBJECTS) -lm

clean:
	rm -f $(OBJECTS) $(TARGET)
