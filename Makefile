CC = gcc
CFLAGS = -O2 -Wall -Wextra -pthread -Isrc
LDFLAGS = -lcrypto -lpthread

SRCS = src/ikcp.c src/encrypt.c src/socks5.c src/tunnel.c src/main.c
OBJS = $(SRCS:.c=.o)
TARGET = bitun

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
