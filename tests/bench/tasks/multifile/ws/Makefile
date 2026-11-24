CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O0 -Iinclude

.PHONY: all test clean

all:
	@echo "targets: test"

test: test_visible.exe
	./test_visible.exe

test_visible.exe: tests/test_visible.c src/idset.c src/idfmt.c include/idset.h
	$(CC) $(CFLAGS) tests/test_visible.c src/idset.c src/idfmt.c -o $@

clean:
	rm -f test_visible.exe
