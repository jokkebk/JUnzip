# Use "make HAVE_ZLIB=1" to enable zlib decompression and compression.
ifdef HAVE_ZLIB
	LIBS = -lz
	DEFS = -DHAVE_ZLIB
endif

# Use "make HAVE_PUFF=1" to use compact puff() by Mark Adler for decompression
ifdef HAVE_PUFF
	PUFF = puff.o
	DEFS = -DHAVE_PUFF
endif

ifeq ($(OS),Windows_NT)
	# MinGW gcc requires disabling MS bitfields for correct struct sizes.
	CFLAGS=-Wall -mno-ms-bitfields $(DEFS)
else
	CFLAGS=-Wall $(DEFS)
endif

CC=gcc
OBJECTS=junzip_test.o junzip_demo.o junzip_dump.o junzip.o jzip_store.o $(PUFF)
COMMANDS=junzip_demo junzip_test junzip_dump jzip_store

all: $(COMMANDS)

clean:
	$(RM) *.o $(DEMO) $(TEST)

junzip_demo: junzip.o junzip_demo.o $(PUFF)
junzip_dump: junzip.o junzip_dump.o $(PUFF)
junzip_test: junzip.o junzip_test.o $(PUFF)
jzip_store: junzip.o jzip_store.o $(PUFF)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJECTS): junzip.h
