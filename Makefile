TARGET_NAME = altair8800
SOURCES     = libretro.c i8080.c

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
  CC      ?= gcc
  CFLAGS   = -fPIC -O2 -Wall -std=c99
  LFLAGS   = -shared -fPIC
  OBJECTS  = libretro.o i8080.o

  all: $(TARGET)
  $(TARGET): $(OBJECTS)
	$(CC) $(LFLAGS) -o $@ $^ -lm
  libretro.o: libretro.c
	$(CC) $(CFLAGS) -c -o $@ $
  i8080.o: i8080.c
	$(CC) $(CFLAGS) -c -o $@ $

# ── macOS (fat binary: x86_64 + arm64 via explicit targets) ──
else ifeq ($(platform), osx)
  TARGET   := $(TARGET_NAME)_libretro.dylib
  CC        = cc
  CFLAGS    = -fPIC -O2 -Wall -std=c99
  LFLAGS    = -dynamiclib -fPIC
  LIB_X86  := $(TARGET_NAME)_libretro.x86_64.dylib
  LIB_ARM  := $(TARGET_NAME)_libretro.arm64.dylib

  all: $(TARGET)

  libretro_x86.o: libretro.c
	$(CC) $(CFLAGS) -arch x86_64 -c -o $@ $
  i8080_x86.o: i8080.c
	$(CC) $(CFLAGS) -arch x86_64 -c -o $@ $
  libretro_arm.o: libretro.c
	$(CC) $(CFLAGS) -arch arm64 -c -o $@ $
  i8080_arm.o: i8080.c
	$(CC) $(CFLAGS) -arch arm64 -c -o $@ $

  $(LIB_X86): libretro_x86.o i8080_x86.o
	$(CC) $(LFLAGS) -arch x86_64 -o $@ $^ -lm
  $(LIB_ARM): libretro_arm.o i8080_arm.o
	$(CC) $(LFLAGS) -arch arm64 -o $@ $^ -lm
  $(TARGET): $(LIB_X86) $(LIB_ARM)
	lipo -create -output $@ $^

# ── Windows (mingw cross) ─────────────────────────────────────
else ifeq ($(platform), win)
  TARGET  := $(TARGET_NAME)_libretro.dll
  CC      ?= x86_64-w64-mingw32-gcc
  CFLAGS   = -O2 -Wall -std=c99
  LFLAGS   = -shared -static-libgcc
  OBJECTS  = libretro.o i8080.o

  all: $(TARGET)
  $(TARGET): $(OBJECTS)
	$(CC) $(LFLAGS) -o $@ $^ -lm
  libretro.o: libretro.c
	$(CC) $(CFLAGS) -c -o $@ $
  i8080.o: i8080.c
	$(CC) $(CFLAGS) -c -o $@ $

endif

.PHONY: all clean
clean:
	rm -f *.o \
	      $(TARGET_NAME)_libretro.so \
	      $(TARGET_NAME)_libretro.dll \
	      $(TARGET_NAME)_libretro.dylib \
	      $(TARGET_NAME)_libretro.x86_64.dylib \
	      $(TARGET_NAME)_libretro.arm64.dylib
