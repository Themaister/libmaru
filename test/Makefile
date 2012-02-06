
TARGETS = bin/test_fifo

CFLAGS += -O3 -g -pthread -std=gnu99 -Wall -pedantic
LDFLAGS += -pthread

all: $(TARGETS)

bin/test_fifo: test_fifo.o ../fifo.o
	mkdir -p bin
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	rm -f *.o
	rm -f $(TARGETS)

.PHONY: all clean
