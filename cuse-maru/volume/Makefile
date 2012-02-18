TARGET := cuse-volume

PREFIX = /usr/local

CFLAGS += -O3 -g -std=gnu99 -Wall

SOURCES := $(wildcard *.c)
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

