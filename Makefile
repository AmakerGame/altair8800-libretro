TARGET_NAME = altair8800

ifeq ($(platform),)
platform = unix
ifeq ($(shell uname -a),)
   platform = win
endif
endif

ifeq ($(platform), unix)
   TARGET := $(TARGET_NAME)_libretro.so
   fpic := -fPIC
   SHARED := -shared
else ifeq ($(platform), win)
   TARGET := $(TARGET_NAME)_libretro.dll
   SHARED := -shared -static-libgcc -static-libstdc++
else ifeq ($(platform), osx)
   TARGET := $(TARGET_NAME)_libretro.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   archs = -arch x86_64 -arch arm64
   CFLAGS += $(archs)
   LDFLAGS += $(archs)
endif

CC = gcc
CFLAGS += $(fpic) -O3 -Wall
OBJECTS = libretro.o i8080.o

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(SHARED) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) -lm

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)