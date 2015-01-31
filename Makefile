CC=gcc
LIBS = -lz
OBJECTS=junzip_test.o junzip_demo.o junzip.o

ifeq ($(OS),Windows_NT)
	# MinGW gcc requires disabling MS bitfields
	CFLAGS=-Wall -mno-ms-bitfields
	DEMO=junzip_demo.exe
	TEST=junzip_test.exe
else
	CFLAGS=-Wall
	DEMO=junzip_demo
	TEST=junzip_test
endif

all: $(DEMO)

run: $(DEMO)
	./$^ test.zip
	
test: $(TEST)
	./$^

clean:
	$(RM) *.o $(DEMO) $(TEST)

$(DEMO): junzip.o junzip_demo.o
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

$(TEST): junzip.o junzip_test.o
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJECTS): junzip.h
