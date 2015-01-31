CC=gcc
CFLAGS=-Wall
LIBS = -lz
DEMO=junzip_demo
TEST=junzip_test
OBJECTS=junzip_test.o junzip_demo.o junzip.o

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
