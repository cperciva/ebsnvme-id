PROG	=	ebsnvme
SRCS	=	main.c
MAN	=	ebsnvme.8
WARNS	?=	6
BINDIR	?=	/usr/local/sbin
MANDIR	?=	/usr/local/man/man
LINKS	=	${BINDIR}/ebsnvme ${BINDIR}/ebsnvme-id \
		${BINDIR}/ebsnvme ${BINDIR}/ebsnvme-stats
MLINKS	=	ebsnvme.8 ebsnvme-id.8 \
		ebsnvme.8 ebsnvme-stats.8

.include <bsd.prog.mk>
