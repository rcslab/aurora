KMOD	= slos
DPSRCS = offset.inc
SRCS	= slos.c slos_inode.c slos_io.c

SRCS	+= slsfs_subr.c slsfs_vnops.c slsfs_vfsops.c slsfs_dir.c \
	  slsfs_buf.c slsfs_alloc.c slsfs_btree.c vnode_if.h

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include
CLEANFILES = .depend*
WITH_CTF = 1

.ifdef SLOS_TEST
DEBUG_FLAGS += -DSLOS_TEST
.endif

.ifdef PERF
DEBUG_FLAGS += -g3 -O2 -DKTR
.endif

.ifdef FASTDBG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DKTR
WITH_CTF = 1
.endif

.ifdef SLOWDBG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_LOCKS -DKTR -DWITNESS
.endif

.include <bsd.kmod.mk>
