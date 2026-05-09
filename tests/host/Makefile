# Host-native unit tests for Mazu network stack (Task 9e).
#
# Compiles and runs on the build host -- no QEMU, no cross-compiler.
# Shadow headers in tests/host/mazu/ override arch-specific RISC-V headers
# while real kernel headers from include/mazu/ provide type definitions.
#
# Usage: make            (builds and runs tests)
#        make clean       (removes build artifacts)

CC       := cc
CFLAGS   := -std=gnu99 -Wall -Wextra -Werror -Wno-keyword-macro \
            -I. -I../../include -D__DEBUG__=0 -D__BASENAME__=\"test\"
SRCS     := test_main.c test_tcp_seq.c test_tcp_hash.c test_http_parse.c
TARGET   := test_host

.PHONY: all clean

all: $(TARGET)
	@echo "=== Host-native tests ==="
	@./$(TARGET)

$(TARGET): $(SRCS)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -o $@ $^

clean:
	@rm -f $(TARGET)
