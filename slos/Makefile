
.PATH: ./ ../slfs

KMOD	= slos
DPSRCS = offset.inc

SRCS	= slos.c slos_alloc.c slos_btree.c slos_inode.c slos_io.c

SRCS	+= slsfs_subr.c slsfs_vnops.c slsfs_vfsops.c slsfs_dir.c \
	  slsfs_buf.c vnode_if.h

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include
CLEANFILES = .depend*
WITH_CTF = 1

.ifdef SLOS_TEST
DEBUG_FLAGS += -DSLOS_TEST
.endif

.ifdef PERF
DEBUG_FLAGS += -g3 -O2 -DKTR -DDB
.endif

.ifdef FASTDBG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DKTR -DDDB
WITH_CTF = 1
.endif

.ifdef SLOWDBG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_LOCKS -DKTR -DWITNESS -DDDB
.endif

.include <bsd.kmod.mk>
