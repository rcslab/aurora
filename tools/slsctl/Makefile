PROG= slsctl
SRCS= slsctl.c attach.c partadd.c partdel.c \
	checkpoint.c restore.c epoch.c spawn.c
MAN=

LDADD= -lsls -lsbuf
LDFLAGS= -L../../libsls
CFLAGS= -I../../include

.include <bsd.prog.mk>
