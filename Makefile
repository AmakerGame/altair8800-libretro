TARGET_NAME = altair8800
CC         ?= gcc
CFLAGS      = -fPIC -O2 -Wall -std=c99
OBJECTS     = libretro.o i8080.o

all: $(TARGET_NAME)_libretro.so

$(TARGET_NAME)_libretro.so: $(OBJECTS)
	$(CC) -shared -fPIC -o $@ $^ -lm

libretro.o: libretro.c
	$(CC) $(CFLAGS) -c -o $@ $

i8080.o: i8080.c
	$(CC) $(CFLAGS) -c -o $@ $

clean:
	rm -f $(OBJECTS) $(TARGET_NAME)_libretro.so

.PHONY: all clean
