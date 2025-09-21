CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pedantic -O2
TARGET = epoll_server
SOURCES = epoll_server.c
OBJS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS) 

.PHONY: all clean