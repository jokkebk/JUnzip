# Use "make HAVE_ZLIB=1" to enable zlib decompression and compression.
ifdef HAVE_ZLIB
	# You could also use -lz but then we'll need custom compile cmds
	LIBS = ../zlib/libz.a
	DEFS = -DHAVE_ZLIB -I../zlib
endif

# Use "make HAVE_LIBS=1" to use compact puff() by Mark Adler for decompression
ifdef HAVE_PUFF
	LIBS = puff.o
	DEFS = -DHAVE_PUFF
endif

ifeq ($(OS),Windows_NT)
	# MinGW gcc requires disabling MS bitfields for correct struct sizes.
	CFLAGS=-Wall -mno-ms-bitfields $(DEFS)
else
	CFLAGS=-Wall $(DEFS)
endif

CC=gcc
OBJECTS=junzip_test.o junzip_demo.o junzip_dump.o junzip.o jzip_demo.o $(LIBS)
COMMANDS=junzip_demo junzip_test junzip_dump jzip_demo

all: $(COMMANDS)

clean:
	$(RM) *.o $(DEMO) $(TEST)

junzip_demo: junzip.o junzip_demo.o $(LIBS)
junzip_dump: junzip.o junzip_dump.o $(LIBS)
junzip_test: junzip.o junzip_test.o $(LIBS)
jzip_demo: junzip.o jzip_demo.o $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJECTS): junzip.h
