KMOD	= slos
DPSRCS = offset.inc
SRCS	= slos.c slos_alloc.c slos_bnode.c slos_bootalloc.c \
	    slos_btree.c slos_inode.c slos_io.c slos_record.c 

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include

.ifdef WITH_DEBUG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DWITNESS -DDEBUG_LOCKS
WITH_CTF = 1
.endif

SRCS	= slos_alloc.c slos_bootalloc.c slos_btree.c slos_bnode.c \
	  slos_record.c slos_inode.c slos_io.c slos.c vnode_if.h
CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include 

.include <bsd.kmod.mk>
