CC=ia16-elf-gcc
CFLAGS=-melks -mcmodel=small -march=i8088

all:
	$(CC) smolnes-elks.c $(CFLAGS) -o smolnes-elks

clean:
	rm -f smolnes-elks
