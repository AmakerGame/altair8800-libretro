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
  TARGET  := $(TARGET_NAME)_libretro.so
  FPIC    := -fPIC
  SHARED  := -shared

# ── macOS (universal binary x86_64 + arm64) ──────────────────
else ifeq ($(platform), osx)
  TARGET  := $(TARGET_NAME)_libretro.dylib
  FPIC    := -fPIC
  SHARED  := -dynamiclib
  ARCHS   := -arch x86_64 -arch arm64
  CC      := cc

# ── Windows (cross-compile from Linux via mingw) ─────────────
else ifeq ($(platform), win)
  TARGET  := $(TARGET_NAME)_libretro.dll
  FPIC    :=
  SHARED  := -shared -static-libgcc
  CC      ?= x86_64-w64-mingw32-gcc
endif

CC     ?= gcc
CFLAGS  = $(FPIC) $(ARCHS) -O3 -Wall -std=c99
LDFLAGS = $(ARCHS)
OBJECTS = libretro.o i8080.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(SHARED) $(LDFLAGS) -o $@ $(OBJECTS) -lm

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $

clean:
	rm -f $(OBJECTS) $(TARGET)
