all: paster2

paster2: crc.c crc.h zutil.c zutil.h paster2.c paster2.h
	gcc -std=c99 -o paster2 crc.c crc.h zutil.c zutil.h paster2.c paster2.h -lz -pthread -lcurl

clean:
	rm -f all.png paster2
