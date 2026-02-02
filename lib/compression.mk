# Makefile for compression libraries
# Include from main cmdstan makefile or run directly with: make -f lib/compression.mk

CC ?= gcc
CFLAGS ?= -O3 -fPIC

.PHONY: all clean lz4 zstd

all: lz4 zstd

lz4: lib/lz4/lz4.o

zstd: lib/zstd/zstd.o

lib/lz4/lz4.o: lib/lz4/lz4.c lib/lz4/lz4.h
	$(CC) $(CFLAGS) -c $< -o $@

lib/zstd/zstd.o: lib/zstd/zstd.c lib/zstd/zstd.h
	$(CC) $(CFLAGS) -DZSTD_DISABLE_ASM -c $< -o $@

clean:
	rm -f lib/lz4/lz4.o lib/zstd/zstd.o
