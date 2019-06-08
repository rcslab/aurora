KMOD	= sls
SRCS	= sls_ioctl.c sls_mem.c sls_proc.c sls_load.c sls_dump.c sls_file.c \
	    sls_rest.c sls_fd.c sls_path.c sls_ckpt.c sls_process.c \
	    sls_mosd.c imported_sls.c sls_objtable.c vnode_if.h
CFLAGS	+= -DKLD_TIED -DGENOFFSET -I../include

.include <bsd.kmod.mk>
