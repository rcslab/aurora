KMOD	= slsfs
DPSRCS = offset.inc
SRCS	= slsfs_subr.c slsfs_vnops.c slsfs_vfsops.c slsfs_dir.c \
	  slsfs_buf.c slsfs_node.c vnode_if.h 

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include

.ifdef WITH_DFLAGS
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_LOCKS
WITH_CTF = 1
.endif

.ifdef WITH_DEBUG
DEBUG_FLAGS += -DWITH_DEBUG
.endif

.ifdef WITNESS
DEBUG_FLAGS += -DWITNESS
.endif

.include <bsd.kmod.mk>
