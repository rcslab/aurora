KMOD	= slos
DPSRCS = offset.inc
SRCS	= slos.c slos_alloc.c slos_bnode.c slos_bootalloc.c \
	    slos_btree.c slos_inode.c slos_io.c slos_record.c 

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include
CLEANFILES = .depend*
WITH_CTF = 1

.ifdef WITH_DFLAGS
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_LOCKS
.endif

.ifdef WITH_DEBUG
DEBUG_FLAGS += -DWITH_DEBUG
.endif

.ifdef WITNESS
DEBUG_FLAGS += -DWITNESS
.endif


.include <bsd.kmod.mk>
