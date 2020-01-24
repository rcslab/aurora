KMOD	= sls
CLEANFILES = .depend*
DPSRCS 	= offset.inc
SRCS	= sls_ioctl.c sls_vmspace.c sls_vmobject.c sls_proc.c sls_load.c \
	    sls_rest.c sls_file.c sls_path.c sls_ckpt.c \
	    sls_kqueue.c sls_pipe.c sls_socket.c \
	    sls_partition.c sls_table.c sls_kv.c sls_sysv.c \
	    sls_pts.c sls_vnode.c sls_posixshm.c \
	    imported_sls.c vnode_if.h

CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include -O0 -g -g3 \
    -DINVARIANTS -DINVARIANT_SUPPORT 

.include <bsd.kmod.mk>
