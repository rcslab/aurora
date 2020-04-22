KMOD	= slos
DPSRCS = offset.inc
SRCS	= slos.c slos_alloc.c slos_bnode.c slos_bootalloc.c \
	    slos_btree.c slos_inode.c slos_io.c slos_record.c slsfs_btree.c

SRCS	+= slsfs_subr.c slsfs_vnops.c slsfs_vfsops.c slsfs_dir.c \
	  slsfs_buf.c slsfs_alloc.c vnode_if.h 

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include
CLEANFILES = .depend*
WITH_CTF = 1

.ifdef WITH_DFLAGS
DEBUG_FLAGS += -g -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_LOCKS
.endif

.ifdef WITH_DEBUG
DEBUG_FLAGS += -DWITH_DEBUG
.endif

.ifdef WITNESS
DEBUG_FLAGS += -DWITNESS
.endif


.include <bsd.kmod.mk>
