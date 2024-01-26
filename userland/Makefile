all: userland-example

CFLAGS += -Wall -Wunused -g
LDFLAGS += -luring

userland-example: main.c
	gcc $(CFLAGS) $(LDFLAGS) -o $@ $<

install: userland-example
	ls -lh
	mkdir -pv ${out}/bin
	cp -v userland-example ${out}/bin/
