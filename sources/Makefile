#
#	Makefile for sleepwatcher
#
#	21.11.2004 bb	created
#	05.05.2005 bb	removed Carbon framework, target clean added
#	23.04.2006 bb	produce a Universal binary of sleepwatcher
#	02.04.2010 bb	-macosx-version-min=10.3 => 10.4
#

CFLAGS_PPC= -O3 -prebind -mmacosx-version-min=10.4 -mtune=G4 -arch ppc
CFLAGS_X86= -O3 -prebind -mmacosx-version-min=10.4 -mtune=pentium -arch i386
LIBS= -framework IOKit -framework CoreFoundation

BINDIR=/usr/local/sbin
MANDIR=/usr/local/man

sleepwatcher: sleepwatcher.c
	$(CC) $(CFLAGS_PPC) -o sleepwatcher.ppc sleepwatcher.c $(LIBS)
	$(CC) $(CFLAGS_X86) -o sleepwatcher.x86 sleepwatcher.c $(LIBS)
	lipo -create sleepwatcher.ppc sleepwatcher.x86 -output sleepwatcher
	rm -f sleepwatcher.ppc sleepwatcher.x86

install: sleepwatcher sleepwatcher.8
	mkdir -p $(BINDIR)
	install -o root -g wheel -m 755 sleepwatcher $(BINDIR)
	mkdir -p $(MANDIR)/man8
	install -o root -g wheel -m 644 sleepwatcher.8 $(MANDIR)/man8

clean:
	rm -f sleepwatcher
