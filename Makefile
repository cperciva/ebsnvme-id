PROG	=	ebsnvme-id
SRCS	=	main.c
MAN	=	ebsnvme-id.8
WARNS	?=	6
BINDIR	?=	/usr/local/sbin
MANDIR	?=	/usr/local/man/man

.include <bsd.prog.mk>
