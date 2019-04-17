KMOD	= sls
SRCS	= sls_ioctl.c sls_mem.c sls_proc.c sls_dump.c sls_file.c \
	    sls_rest.c sls_snapshot.c sls_fd.c path.c sls_ckpt.c sls_process.c \
	    sls_mosd.c sls_osd.c vnode_if.h
CFLAGS	+= -DKLD_TIED -DGENOFFSET -I../include

.include <bsd.kmod.mk>
