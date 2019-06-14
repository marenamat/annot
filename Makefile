annot: annot.c
	gcc -std=c11 -lutil -O2 $< -o $@
