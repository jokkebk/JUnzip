CC=gcc
# If you want compression support, uncomment LIBS and comment DEFS below
#LIBS = -lz
DEFS = -DNOZLIB
OBJECTS=junzip_test.o junzip_demo.o junzip_dump.o junzip.o jzip_store.o

ifeq ($(OS),Windows_NT)
	# MinGW gcc requires disabling MS bitfields
	CFLAGS=-Wall -mno-ms-bitfields $(DEFS)
	DEMO=junzip_demo.exe
	TEST=junzip_test.exe
	DUMP=junzip_dump.exe
	STORE=jzip_store.exe
else
	CFLAGS=-Wall $(DEFS)
	DEMO=junzip_demo
	TEST=junzip_test
	DUMP=junzip_dump
	STORE=jzip_store
endif

all: $(DEMO)

run: $(DEMO)
	./$^ test.zip
	
test: $(TEST)
	./$^

dump: $(DUMP)
	./$^

store: $(STORE)
	./$^

clean:
	$(RM) *.o $(DEMO) $(TEST)

$(DEMO): junzip.o junzip_demo.o
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

$(TEST): junzip.o junzip_test.o
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

$(DUMP): junzip.o junzip_dump.o
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

$(STORE): junzip.o jzip_store.o
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJECTS): junzip.h
