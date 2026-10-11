TARGET_NAME = altair8800
CC         ?= gcc
CFLAGS     ?= -O2
CFLAGS     += -Wall -std=c99
OBJECTS     = libretro.o i8080.o

# ---- platform detection ----
ifeq ($(OS),Windows_NT)
   EXT      = dll
   LDFLAGS += -shared -static-libgcc
   CFLAGS  += -U_FORTIFY_SOURCE
else
   UNAME := $(shell uname -s)
   ifeq ($(UNAME),Darwin)
      EXT      = dylib
      CFLAGS  += -fPIC
      LDFLAGS += -dynamiclib
   else
      EXT      = so
      CFLAGS  += -fPIC
      LDFLAGS += -shared
   endif
endif

TARGET = $(TARGET_NAME)_libretro.$(EXT)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

%.o: %.c i8080.h libretro.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
