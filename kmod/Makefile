KMOD	= sls
CLEANFILES = .depend*
DPSRCS 	= offset.inc
SRCS	= sls_ioctl.c sls_mem.c sls_proc.c sls_load.c sls_dump.c sls_channel.c \
	    sls_rest.c sls_fd.c sls_path.c sls_ckpt.c sls_process.c \
	    imported_sls.c sls_objtable.c vnode_if.h 
CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include -g
WITH_CTF = 1

.include <bsd.kmod.mk>
