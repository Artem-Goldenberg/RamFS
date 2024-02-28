
CFLAGS += -I/usr/local/include/fuse
CFLAGS += -D_FILE_OFFSET_BITS=64
CFLAGS += -std=c17
LDFLAGS += -lfuse

main: main.c Filesystem.c

launch: main
	./main -d RAM

clean:
	$(RM) main