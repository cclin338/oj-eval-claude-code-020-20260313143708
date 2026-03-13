.PHONY: all
all:
	gcc -o code main.c buddy.c -O2 -Wno-int-conversion

.PHONY: test
test:
	gcc -o test main.c buddy.c -g -Wno-int-conversion