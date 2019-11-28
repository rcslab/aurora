KMOD	= slos
SRCS	= slos_alloc.c slos_bootalloc.c slos_btree.c slos_bnode.c \
	  slos_record.c slos_inode.c slos_io.c slos.c vnode_if.h
CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include -O0 -g -g3 #\
    -DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_LOCKS
CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -DGENOFFSET -g
WITH_CTF = 1

.include <bsd.kmod.mk>
