KMOD	= slos
CLEANFILES = .depend*
DPSRCS 	= offset.inc
SRCS	= slos_alloc.c slos_bootalloc.c slos_btree.c slos_bnode.c \
	  slos_record.c slos_inode.c slos_io.c slos.c vnode_if.h
CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include -O0 -g -g3 \
    -DINVARIANTS -DINVARIANT_SUPPORT 
WITH_CTF = 1

.include <bsd.kmod.mk>
