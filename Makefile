SHELL	= /bin/sh

CC	= cc
CFLAGS	= -pipe -Os -Wall -W -I/usr/local/include
LDFLAGS	= -s -L/usr/local/lib
LIBS	= -lowfat

ALL = minicron

all: $(ALL)

minicron: minicron.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o ${.TARGET} ${.ALLSRC} $(LIBS)

clean:
	rm -f a.out *.o *~ $(ALL) *.tar.bz2 *.tar.gz Z*

