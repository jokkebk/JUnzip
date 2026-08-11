# Build the small command-line tools and the in-memory security regression
# suite. Run `make clean` when switching HAVE_ZLIB/HAVE_PUFF backends.

CC ?= gcc
CPPFLAGS +=
CFLAGS += -Wall -Wextra
LDFLAGS +=
LDLIBS +=

ifeq ($(OS),Windows_NT)
    CFLAGS += -mno-ms-bitfields
endif

ifdef HAVE_ZLIB
    CPPFLAGS += -DHAVE_ZLIB
    LDLIBS += -lz
endif

ifdef HAVE_PUFF
    CPPFLAGS += -DHAVE_PUFF
    PUFF_OBJECT = puff.o
endif

ifdef SANITIZE
    SANITIZE_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all
    CFLAGS += $(SANITIZE_FLAGS) -fstack-protector-strong
    LDFLAGS += $(SANITIZE_FLAGS)
endif

COMMON_OBJECTS = junzip.o $(PUFF_OBJECT)
COMMANDS = junzip_demo junzip_test junzip_dump jzip_demo junzip_security_test

.PHONY: all clean check check-puff check-zlib sanitize fuzz fuzz-smoke

all: $(COMMANDS)

clean:
	$(RM) *.o $(COMMANDS) junzip_fuzz junzip_fuzz_smoke

check: junzip_test junzip_security_test
	./junzip_test
	./junzip_security_test

check-puff:
	$(MAKE) clean
	$(MAKE) HAVE_PUFF=1 check

check-zlib:
	$(MAKE) clean
	$(MAKE) HAVE_ZLIB=1 check

sanitize:
	$(MAKE) clean
	$(MAKE) SANITIZE=1 HAVE_PUFF=1 check
	$(MAKE) clean
	$(MAKE) SANITIZE=1 HAVE_ZLIB=1 check

fuzz:
	clang -DHAVE_PUFF -Wall -Wextra -fsanitize=fuzzer,address,undefined \
		junzip_fuzz.c junzip.c puff.c -o junzip_fuzz

fuzz-smoke:
	clang -DJUNZIP_FUZZ_STANDALONE -DHAVE_PUFF -Wall -Wextra \
		-fsanitize=address,undefined junzip_fuzz.c junzip.c puff.c \
		-o junzip_fuzz_smoke

$(COMMANDS): %: %.o $(COMMON_OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(COMMANDS:%=%.o) $(COMMON_OBJECTS): junzip.h
