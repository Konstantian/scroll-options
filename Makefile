CC = x86_64-w64-mingw32-gcc
WINDRES = x86_64-w64-mingw32-windres
CFLAGS = -Os -s -Wall -Wextra -municode -mwindows -static

# Release builds pass VERSION=1.0.<build>.
VERSION = 1.0.0
comma := ,
VERSION_NUM = $(subst .,$(comma),$(VERSION)),0

scroll-options.exe: scroll-options.c version.o
	$(CC) $(CFLAGS) -DVERSION='"$(VERSION)"' -o $@ $^ -lshell32 -ldwmapi

version.o: scroll-options.rc FORCE
	$(WINDRES) -DVERSION='\"$(VERSION)\"' -DVERSION_NUM=$(VERSION_NUM) $< $@

clean:
	rm -f scroll-options.exe version.o

FORCE:
.PHONY: clean FORCE
