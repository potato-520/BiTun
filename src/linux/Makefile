CC = gcc
CFLAGS = -O2 -Wall -Wextra -pthread -I..
LDFLAGS = -lcrypto -lpthread

OBJS = ikcp.o encrypt.o socks5.o tunnel.o bitun_osal.o main.o fec.o
TARGET = bitun

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

ikcp.o: ../ikcp.c
	$(CC) $(CFLAGS) -c -o $@ $<

encrypt.o: ../encrypt.c
	$(CC) $(CFLAGS) -c -o $@ $<

socks5.o: ../socks5.c
	$(CC) $(CFLAGS) -c -o $@ $<

tunnel.o: ../tunnel.c
	$(CC) $(CFLAGS) -c -o $@ $<

fec.o: ../fec.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
