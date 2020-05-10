KMOD	= slos
DPSRCS = offset.inc
SRCS	= slos.c slos_inode.c slos_io.c

SRCS	+= slsfs_subr.c slsfs_vnops.c slsfs_vfsops.c slsfs_dir.c \
	  slsfs_buf.c slsfs_alloc.c slsfs_btree.c vnode_if.h

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include
CLEANFILES = .depend*
WITH_CTF = 1

.ifdef WITH_DFLAGS
WITH_INVARIANTS=1
WITH_DEBUG_LOCKS=1
.endif

.ifdef WITH_INVARIANTS
DEBUG_FLAGS += -g -O0 -DINVARIANTS -DINVARIANT_SUPPORT
.endif

.ifdef WITH_DEBUG_LOCKS
DEBUG_FLAGS += -DDEBUG_LOCKS
.endif

.ifdef WITH_KTR
DEBUG_FLAGS += -DKTR
.endif

.ifdef WITH_DEBUG
DEBUG_FLAGS += -DWITH_DEBUG
.endif

.ifdef WITNESS
DEBUG_FLAGS += -DWITNESS
.endif


.include <bsd.kmod.mk>
