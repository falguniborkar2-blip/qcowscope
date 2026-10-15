CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -Wshadow -Wformat=2 \
           -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64
SRC     := src/main.c src/qcow2.c src/snapshot.c
HDR     := src/qcow2.h

.PHONY: all test asan clean

all: qcowscope

qcowscope: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

# Build with AddressSanitizer + UndefinedBehaviorSanitizer to catch memory bugs.
qcowscope-asan: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -O1 -fno-omit-frame-pointer \
	      -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $(SRC)

test: qcowscope
	python3 -m pytest -q tests

asan: qcowscope-asan
	QCOWSCOPE_BIN=./qcowscope-asan python3 -m pytest -q tests

clean:
	rm -f qcowscope qcowscope-asan
