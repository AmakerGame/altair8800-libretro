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
  SHARED  := -shared -fPIC

# ── macOS (universal binary x86_64 + arm64) ──────────────────
else ifeq ($(platform), osx)
  TARGET  := $(TARGET_NAME)_libretro.dylib
  FPIC    := -fPIC
  SHARED  := -dynamiclib -fPIC
  ARCHS   := -arch x86_64 -arch arm64
  CC      := cc

# ── Windows (cross-compile from Linux via mingw) ─────────────
else ifeq ($(platform), win)
  TARGET  := $(TARGET_NAME)_libretro.dll
  FPIC    :=
  SHARED  := -shared -static-libgcc -static-libstdc++
  CC      ?= x86_64-w64-mingw32-gcc
  # Disable _FORTIFY_SOURCE: mingw has no __printf_chk
  EXTRA   := -D_FORTIFY_SOURCE=0 -U_FORTIFY_SOURCE
endif

CC      ?= gcc
CFLAGS   = $(FPIC) $(ARCHS) -O2 -Wall -std=c99 $(EXTRA)
OBJECTS  = libretro.o i8080.o

.PHONY: all clean

all: $(TARGET)

# Link: only SHARED + ARCHS, no CFLAGS to avoid duplicate -arch
$(TARGET): $(OBJECTS)
	$(CC) $(SHARED) $(ARCHS) -o $@ $(OBJECTS) -lm

# Compile each .c separately with full CFLAGS including ARCHS
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $

clean:
	rm -f $(OBJECTS) $(TARGET)
