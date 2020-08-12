#include <sys/param.h>

#include <sys/buf.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/libkern.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/priority.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include <slos.h>
#include <slos_inode.h>
#include <slos_btree.h>
#include <slos_record.h>

#include "slos_io.h"
#include "slosmm.h"

MALLOC_DEFINE(M_SLOS, "slos", "SLOS");
MALLOC_DEFINE(M_SLOS_SB, "slos superblock", "SLOSS");

/* We have only one SLOS currently, make it global. */
struct slos slos;
