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

  OBJECTS  = $(SOURCES:.c=.o)

  all: $(TARGET)
  $(TARGET): $(OBJECTS)
	$(CC) $(LFLAGS) -o $@ $(OBJECTS) -lm
  %.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $

# ── macOS: compile twice, lipo into fat dylib ────────────────
else ifeq ($(platform), osx)
  TARGET  := $(TARGET_NAME)_libretro.dylib
  CC       = cc
  CFLAGS   = -fPIC -O2 -Wall -std=c99
  LFLAGS   = -dynamiclib -fPIC

  OBJ_X86  = $(SOURCES:.c=.x86_64.o)
  OBJ_ARM  = $(SOURCES:.c=.arm64.o)
  LIB_X86  = $(TARGET:.dylib=.x86_64.dylib)
  LIB_ARM  = $(TARGET:.dylib=.arm64.dylib)

  all: $(TARGET)

  %.x86_64.o: %.c
	$(CC) $(CFLAGS) -arch x86_64 -c -o $@ $

  %.arm64.o: %.c
	$(CC) $(CFLAGS) -arch arm64 -c -o $@ $

  $(LIB_X86): $(OBJ_X86)
	$(CC) $(LFLAGS) -arch x86_64 -o $@ $(OBJ_X86) -lm

  $(LIB_ARM): $(OBJ_ARM)
	$(CC) $(LFLAGS) -arch arm64 -o $@ $(OBJ_ARM) -lm

  $(TARGET): $(LIB_X86) $(LIB_ARM)
	lipo -create -output $@ $(LIB_X86) $(LIB_ARM)

# ── Windows (mingw cross) ─────────────────────────────────────
else ifeq ($(platform), win)
  TARGET  := $(TARGET_NAME)_libretro.dll
  CC      ?= x86_64-w64-mingw32-gcc
  # CPPFLAGS overrides compiler specs (-D_FORTIFY_SOURCE=3 baked into ubuntu gcc)
  CPPFLAGS = -U_FORTIFY_SOURCE
  CFLAGS   = -O2 -Wall -std=c99
  LFLAGS   = -shared -static-libgcc

  OBJECTS  = $(SOURCES:.c=.o)

  all: $(TARGET)
  $(TARGET): $(OBJECTS)
	$(CC) $(LFLAGS) -o $@ $(OBJECTS) -lm
  %.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $

endif

.PHONY: all clean
clean:
	rm -f *.o *.x86_64.o *.arm64.o \
	      *.x86_64.dylib *.arm64.dylib \
	      $(TARGET_NAME)_libretro.so \
	      $(TARGET_NAME)_libretro.dll \
	      $(TARGET_NAME)_libretro.dylib
