CC = x86_64-w64-mingw32-gcc
CFLAGS = -Os -s -Wall -Wextra -municode -mwindows -static

smooth-scroll.exe: smooth-scroll.c
	$(CC) $(CFLAGS) -o $@ $< -lshell32

clean:
	rm -f smooth-scroll.exe

.PHONY: clean
