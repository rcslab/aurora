KMOD	= sls
SRCS	= sls_ioctl.c sls_mem.c sls_proc.c sls_dump.c sls_file.c \
	    sls_restore.c sls_process.c sls_fd.c path.c sls_shadow.c \
	    sls_checkpoint.c \
	    vnode_if.h
CFLAGS	+= -DKLD_TIED -DGENOFFSET -I../include

.include <bsd.kmod.mk>
