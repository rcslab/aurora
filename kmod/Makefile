CFLAGS	+= -DKLD_TIED -DGENOFFSET
KMOD	= slsmm
SRCS	= slsmm.c bufc.c memckpt.c cpuckpt.c dump.c \
	    hash.c fd.c path.c vnode_if.h \
	    backends/fileio.c backends/worker.c \
	    backends/file.c backends/nvdimm.c

.include <bsd.kmod.mk>
