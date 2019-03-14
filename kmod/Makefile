KMOD	= slsmm
SRCS	= slsmm.c bufc.c memckpt.c cpuckpt.c dump.c \
	    hash.c fd.c path.c vnhash.c vnode_if.h \
	    backends/fileio.c backends/worker.c \
	    backends/file.c backends/nvdimm.c
CFLAGS	+= -DKLD_TIED -DGENOFFSET -I../include

.include <bsd.kmod.mk>
