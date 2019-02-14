.PATH:  ${.CURDIR}
CFLAGS	+= -DKLD_TIED -DGENOFFSET
KMOD	= slsmm
SRCS	= slsmm.c fileio.c memckpt.c cpuckpt.c dump.c hash.c fd.c path.c vnode_if.h 

.include <bsd.kmod.mk>
