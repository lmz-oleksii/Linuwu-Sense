CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -std=c11
LDFLAGS := -lyaml
TARGET  := linuwu-sensed
SRC     := linuwu_sensed.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
