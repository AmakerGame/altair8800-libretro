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
else
   TARGET := $(TARGET_NAME)_libretro.dll
   SHARED := -shared -static-libgcc -static-libstdc++
endif

CC = gcc
CFLAGS += $(fpic) -O3 -Wall

OBJECTS = libretro.o i8080.o

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(SHARED) $(CFLAGS) -o $@ $(OBJECTS) -lm

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)