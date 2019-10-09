KMOD	= sls
CLEANFILES = .depend*
DPSRCS 	= offset.inc
SRCS	= sls_ioctl.c sls_mem.c sls_proc.c sls_load.c \
	    sls_rest.c sls_fd.c sls_path.c sls_ckpt.c sls_process.c \
	    slstable.c slskv.c imported_sls.c vnode_if.h
CFLAGS	+= -DKDTRACE_HOOKS -DKLD_TIED -I../include -g 
WITH_CTF = 1

.include <bsd.kmod.mk>
