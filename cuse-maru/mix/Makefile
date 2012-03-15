TARGET := cuse-mix

PREFIX = /usr/local

CFLAGS += -O2 -g -pthread -std=gnu99 -Wall $(shell pkg-config fuse libmaru --cflags)
LDFLAGS += -pthread $(shell pkg-config fuse libmaru --libs) -lm

SOURCES := $(wildcard *.c)
OBJECTS := $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	rm -f $(OBJECTS)
	rm -f $(TARGET)

install:
	install -m755 $(TARGET) $(PREFIX)/bin

.PHONY: all clean install

