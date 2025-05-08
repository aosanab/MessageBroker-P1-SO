CC = gcc
CFLAGS = -Wall -pthread
SRC_DIR = src
TARGETS = broker producer consumer

all: $(TARGETS)

broker: $(SRC_DIR)/broker.c
	$(CC) $(CFLAGS) -o $@ $<

producer: $(SRC_DIR)/producer.c
	$(CC) $(CFLAGS) -o $@ $<

consumer: $(SRC_DIR)/consumer.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean