CC      := gcc
CFLAGS  := -Wall -O2
TARGET  := linuwu-sensed
SRC     := fan_daemon.c

PREFIX     := /usr/local
BINDIR     := $(PREFIX)/bin
SYSCONFDIR := /etc

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET) fan_daemon_c

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(SYSCONFDIR)
	@if [ ! -f $(DESTDIR)$(SYSCONFDIR)/linuwu-sense.conf ]; then \
		install -m 644 linuwu-sense.conf $(DESTDIR)$(SYSCONFDIR)/linuwu-sense.conf; \
		echo "Installed default configuration to $(DESTDIR)$(SYSCONFDIR)/linuwu-sense.conf"; \
	else \
		echo "Configuration file $(DESTDIR)$(SYSCONFDIR)/linuwu-sense.conf already exists, skipping..."; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Uninstalled $(DESTDIR)$(BINDIR)/$(TARGET)"
	@echo "Note: $(DESTDIR)$(SYSCONFDIR)/linuwu-sense.conf was intentionally kept."
