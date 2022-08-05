# $Id: Makefile 2066 2011-10-26 15:40:28Z jkoshy $

PROG=	mdump
SRCS=	mdump.c addr2line.c

BINDIR=	/usr/local/bin
MANDIR=/usr/local/man/man

CPPFLAGS+= -I /usr/local/include/elftoolchain
CPPFLAGS+= -I /usr/local/include/libdwarf

CFLAGS+=-g3 -O0
CFLAGS+=-Wall -I${.CURDIR}
CFLAGS+=-Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=-Wmissing-declarations
CFLAGS+=-Wshadow -Wpointer-arith -Wcast-qual
CFLAGS+=-Wsign-compare

LDFLAGS+= -L /usr/local/lib/elftoolchain
LDADD=	-lelftc -ldwarf -lelf -lutil

.include <bsd.prog.mk>
