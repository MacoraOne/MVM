CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation
LDLIBS ?= -lm

.PHONY: all test clean
all: vm-test

vm-test: vm.c vm.h test_vm.c
	$(CC) $(CFLAGS) vm.c test_vm.c -o $@ $(LDLIBS)

test: vm-test
	./vm-test

clean:
	rm -f vm-test
