TARGET := cuse-maru

PREFIX = /usr/local

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

install:
	install -m755 $(TARGET) $(PREFIX)/bin

.PHONY: all clean install

