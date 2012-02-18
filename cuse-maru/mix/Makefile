TARGET := cuse-mix

CFLAGS += -O3 -g -pthread -std=gnu99 -Wall $(shell pkg-config fuse --cflags)
LDFLAGS += -pthread $(shell pkg-config fuse --libs) -lsamplerate

SOURCES := $(wildcard *.c) ../../fifo.c
OBJECTS := $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	rm -f $(OBJECTS)
	rm -f $(TARGETS)

.PHONY: all clean

