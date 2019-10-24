KMOD	= sls
CLEANFILES = .depend*
DPSRCS 	= offset.inc
SRCS	= sls_ioctl.c sls_vmspace.c sls_vmobject.c sls_proc.c sls_load.c \
	    sls_rest.c sls_file.c sls_path.c sls_ckpt.c \
	    sls_partition.c sls_table.c sls_kv.c \
	    imported_sls.c vnode_if.h
CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include -g 
WITH_CTF = 1

.include <bsd.kmod.mk>
