KMOD	= sls
CLEANFILES = .depend*
DPSRCS 	= offset.inc
SRCS	= sls_ioctl.c sls_vmspace.c sls_vmobject.c sls_proc.c sls_load.c \
	    sls_rest.c sls_file.c sls_path.c sls_ckpt.c \
	    sls_kqueue.c sls_pipe.c sls_socket.c \
	    sls_partition.c sls_table.c sls_kv.c sls_sysv.c \
	    sls_pts.c sls_vnode.c sls_posixshm.c \
	    imported_sls.c sls_vm.c vnode_if.h
CFLAGS	+= -DKDTRACE_HOOKS -DSMP -DKLD_TIED -I../include -g
CLEANFILES = .depend*
WITH_CTF = 1

.ifdef SLS_MSG
DEBUG_FLAGS += -DSLS_MSG
.endif

.ifdef SLS_TEST
DEBUG_FLAGS += -DSLS_TEST
.endif

.ifdef PERF
DEBUG_FLAGS += -g3 -O2 -DKTR
.endif

.ifdef FASTDBG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_LOCKS -DKTR
.endif

.ifdef SLOWDBG
DEBUG_FLAGS += -g3 -O0 -DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_LOCKS -DKTR -DWITNESS
.endif

.include <bsd.kmod.mk>
