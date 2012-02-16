
TARGETS = bin/test_fifo bin/test_enum

CFLAGS += -O3 -pthread -std=gnu99 -Wall -I.. $(shell pkg-config libusb-1.0 --cflags)
LDFLAGS += -pthread $(shell pkg-config libusb-1.0 --libs) -lrt

all: $(TARGETS)

bin/test_fifo: test_fifo.o ../fifo.o
	mkdir -p bin
	$(CC) -o $@ $^ $(LDFLAGS)

bin/test_enum: test_enum.o ../fifo.o ../libmaru.o
	mkdir -p bin
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	rm -f *.o
	rm -f $(TARGETS)

.PHONY: all clean
