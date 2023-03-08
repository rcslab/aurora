PROG= slsctl
SRCS= slsctl.c attach.c partadd.c partdel.c \
	checkpoint.c restore.c spawn.c \
	listsnaps.c pgresident.c \
	partadd_slos.c partadd_file.c \
	partadd_memory.c partadd_send.c \
	partadd_recv.c
MAN=

LDADD= -lsls -lsbuf -ledit
LDFLAGS= -L../../libsls
CFLAGS= -I../../include

.include <bsd.prog.mk>
