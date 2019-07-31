CC=gcc
OBJECTS=junzip_test.o junzip_demo.o junzip_dump.o junzip.o jzip_store.o
COMMANDS=junzip_demo junzip_test junzip_dump jzip_store

# Use "make HAVE_ZLIB=1" to enable zlib support and compression.
ifdef HAVE_ZLIB
	LIBS = -lz
	DEFS = -DHAVE_ZLIB
endif

ifeq ($(OS),Windows_NT)
	# MinGW gcc requires disabling MS bitfields for correct struct sizes.
	CFLAGS=-Wall -mno-ms-bitfields $(DEFS)
else
	CFLAGS=-Wall $(DEFS)
endif

all: $(COMMANDS)

clean:
	$(RM) *.o $(DEMO) $(TEST)

junzip_demo: junzip.o junzip_demo.o
junzip_dump: junzip.o junzip_dump.o
junzip_test: junzip.o junzip_test.o
jzip_store: junzip.o jzip_store.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJECTS): junzip.h
