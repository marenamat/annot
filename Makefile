#CFLAGS=-ggdb3 -gdwarf-4 -std=c11 -lutil -O2
CFLAGS=-ggdb3 -gdwarf-4 -std=c11
LIBS=-lutil

annot: annot.c Makefile
	gcc $(CFLAGS) $< -o $@ $(LIBS)
