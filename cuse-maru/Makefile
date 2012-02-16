TARGET := cuse-maru

CFLAGS += -O3 -g -pthread -std=gnu99 -Wall -I.. $(shell pkg-config libusb-1.0 fuse --cflags)
LDFLAGS += -pthread $(shell pkg-config libusb-1.0 fuse --libs)

SOURCES := $(wildcard *.c) $(wildcard ../*.c)
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

