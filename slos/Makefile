KMOD	= slos
DPSRCS = offset.inc
SRCS	= slos.c slos_alloc.c slos_bnode.c slos_bootalloc.c \
	    slos_btree.c slos_inode.c slos_io.c slos_record.c \
	    slsfs_subr.c slsfs_vnops.c slsfs_vfsops.c slsfs_dir.c \
	    vnode_if.h 

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include -O0 -g -g3 -DINVARIANTS \
    -DINVARIANT_SUPPORT -DWITNESSS -DDEBUG_LOCKS
DEBUG_FLAGS += -g3
WITH_CTF = 1

.include <bsd.kmod.mk>
