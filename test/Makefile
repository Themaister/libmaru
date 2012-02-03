
TARGETS = test_fifo

CFLAGS += -O0 -g -pthread
LDFLAGS += -pthread

all: $(TARGETS)

test_fifo: test_fifo.o ../fifo.o
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	rm -f *.o
	rm -f $(TARGETS)

.PHONY: all clean
