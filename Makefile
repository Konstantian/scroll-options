CC = x86_64-w64-mingw32-gcc
CFLAGS = -Os -s -Wall -Wextra -municode -mwindows -static

scroll-options.exe: scroll-options.c
	$(CC) $(CFLAGS) -o $@ $< -lshell32 -ldwmapi

clean:
	rm -f scroll-options.exe

.PHONY: clean
