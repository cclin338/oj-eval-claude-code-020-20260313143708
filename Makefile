.PHONY: all
all:
	gcc -o code main.c buddy.c -Wall -O2

.PHONY: test
test:
	gcc -o test main.c buddy.c -Wall -g