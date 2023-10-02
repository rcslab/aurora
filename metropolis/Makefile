KMOD	= metropolis
CLEANFILES = .depend*
DPSRCS 	= offset.inc
SRCS	= metr_ioctl.c vnode_if.h
CFLAGS	+= -DKDTRACE_HOOKS -DSMP -DKLD_TIED -I../include -g
CLEANFILES = .depend*
WITH_CTF = 1

.ifdef PERF
DEBUG_FLAGS += -g3 -O2 -DDDB
.endif

.ifdef FASTDBG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DKTR -DDDB
.endif

.ifdef SLOWDBG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_LOCKS -DKTR -DDDB -DWITNESS
.endif

.include <bsd.kmod.mk>
