CC = g++
# CFLAGS = -Wall -g -std=c++11
TARGET = test
SOURCES = test.cpp ../fiber/fiber.cpp ../scheduler/scheduler.cpp ../thread/thread.cpp
OBJS = $(SOURCES:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^

%.o: %.cpp
	$(CC) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS) 

.PHONY: all clean