KMOD	= slsfs
DPSRCS = offset.inc
SRCS	= slsfs_subr.c slsfs_vnops.c slsfs_vfsops.c slsfs_dir.c \
	    vnode_if.h 

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include

.ifdef WITH_DEBUG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DWITNESS -DDEBUG_LOCKS
.endif

WITH_CTF = 1

.include <bsd.kmod.mk>
