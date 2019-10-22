#include <sys/param.h>

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/capsicum.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/syscallsubr.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>

#include <machine/atomic.h>

#include "slsmm.h"

#include "sls.h"
#include "sls_channel.h"
#include "sls_mem.h"
#include "imported_sls.h"

#include <slos.h>
#include "../slos/slos_inode.h"
#include "../slos/slos_io.h"
#include "../slos/slos_record.h"

extern struct slos slos;
/* 
 * Read len bytes into the buffer pointed 
 * to by addr from file pointer fp.
 */
int
sls_file_read(void *addr, size_t len, struct file *fp)
{
	int error = 0;

	struct uio auio;
	struct iovec aiov;
	bzero(&auio, sizeof(struct uio));
	bzero(&aiov, sizeof(struct iovec));

	aiov.iov_base = (void*)addr;
	aiov.iov_len = len;

	auio.uio_iov = &aiov;
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_iovcnt = 1;
	auio.uio_resid = len;
	auio.uio_td = curthread;

	/* 
	 * XXX The fd argument is bogus, but it's not needed
	 * except for ktrace. Both dofileread() and dofilewrite()
	 * can be split so that the fd-agnostic part can be called
	 * on its own.
	 *
	 * Also, dofileread/dofilewrite are static, so we "import" them
	 * by including them in the imports file.
	 */
	error = dofileread(curthread, 0, fp, &auio, (off_t) -1, 0);
	if (error != 0)
	    SLS_DBG("Error: dofileread failed with code %d\n", error);

	return error;
}

/* 
 * Write len bytes from the buffer pointed 
 * to by addr into file pointer fp.
 */
int
sls_file_write(void *addr, size_t len, struct file *fp)
{
	int error = 0;
	struct uio auio;
	struct iovec aiov;


	bzero(&auio, sizeof(struct uio));
	bzero(&aiov, sizeof(struct iovec));

	aiov.iov_base = (void*)addr;
	aiov.iov_len = len;

	auio.uio_iov = &aiov;
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_iovcnt = 1;
	auio.uio_resid = len;
	auio.uio_td = curthread;

	error = dofilewrite(curthread, 0, fp, &auio, (off_t) -1, 0);
	if (error != 0)
	    SLS_DBG("Error: kern_writev failed with code %d\n", error);

	return error;
}

int
sls_slos_read(void* addr, size_t len, uint64_t type, uint64_t *offset, 
	struct slos_node *vp)
{
	struct iovec aiov;
	struct uio auio;
	uint64_t rno;
	int error;

	/* 
	 * XXX Right now we are using only two types of records
	 * in the SLS - one for the metadata, and one for the
	 * data of the process. That is important because
	 * here we would have normally traversed the backend
	 * and found the starting point of the checkpoint, and
	 * then consecutively read each record separately. Here
	 * we just get the record with the metadata and read it.
	 * When we have multiple records for the metadata, we are
	 * going to have to do more bookkeeping when reading. 
	 */

	/* Get the SLOS record with the metadata. */
	error = slos_lastrno_typed(vp, SLOSREC_PROC, &rno);
	if (error != 0) {
	    return error;
	}

	/* Create the UIO for the disk. */
	aiov.iov_base = addr;
	aiov.iov_len = len;
	slos_uioinit(&auio, *offset, UIO_READ, &aiov, 1);

	/* The read itself. */
	error = slos_rread(vp, rno, &auio);
	if (error != 0)
	    return error;

	/* Increment the offset by the amount of bytes read. */
	*offset += (len - auio.uio_resid);
	
	return 0;
}

/* 
 * Write a new record with type value type into the SLOS inode pointed to
 * by vp, whose data is the buffer of size len pointed to by addr.
 */
int
sls_slos_write(void* addr, size_t len, uint64_t type, uint64_t *offset, 
	struct slos_node *vp)
{
	uint64_t rno;
	int error;
	struct uio auio;
	struct iovec aiov;

	/* Create the SLOS record for the metadata. */
	error = slos_rcreate(vp, type, &rno);
	if (error != 0) {
	    return error;
	}

	/* Create the UIO for the disk. */
	aiov.iov_base = addr;
	aiov.iov_len = len;
	slos_uioinit(&auio, *offset, UIO_WRITE, &aiov, 1);

	/* The write itself. */
	error = slos_rwrite(vp, rno, &auio);
	if (error != 0)
	    return error;

	/* Advance the offset into the record. */
	*offset += (len - auio.uio_resid);

	return 0;
}

/* 
 * Read a buffer buf of length len into the channel. The 
 * channel works as a simple wrapper for the file pointer 
 * in the case of dumping and restoring from a file, but 
 * holds the equivalent state of a file pointer when used
 * to read from the OSD. The reason is that we are issuing
 * consecutive reads into the same record, so we have to
 * track the current offset each time.
 */
int 
sls_read(void *buf, size_t len, uint64_t rtype, struct sls_channel *chan)
{ 
	int error;

	switch (chan->type) {
	case SLS_FILE:
	    error = sls_file_read(buf, len, chan->fp);
	    break;

	case SLS_OSD:
	    error = sls_slos_read(buf, len, rtype, &chan->offset, chan->vp);
	    break;

	default:
	    return EINVAL;
	}

	return error;
}

/* Write a buffer buf of length len into the channel. */
int 
sls_write(void *buf, size_t len, uint64_t rtype, struct sls_channel *chan)
{ 
	int error;

	switch (chan->type) {
	case SLS_FILE:
	    error = sls_file_write(buf, len, chan->fp);
	    break;

	case SLS_OSD:
	    error = sls_slos_write(buf, len, rtype, &chan->offset, chan->vp);
	    break;

	default:
	    return EINVAL;
	}

	return error;
}

/* Create a file-backed SLS channel. */
static int
slschan_initfile(struct sbuf *sb, struct sls_channel *chanp)
{
	struct sls_channel chan;
	struct file *fp;
	int error;
	int flags;
	int fd;
	
	/* The flags for the open call below. */
	flags = O_RDWR | O_CREAT | O_DIRECT;

	/* Get a file descriptor for the file in which we will dump. */
	error = kern_openat(curthread, AT_FDCWD, sbuf_data(sb),
		 UIO_SYSSPACE, flags, S_IRWXU);
	if (error != 0)
	    return error;

	/* 
	 * We get the return value of kern_openat() from the
	 * calling thread like we do for all syscalls.
	 */
	fd = curthread->td_retval[0];

	/* 
	 * We could use the fd for checkpointing, but we can't do so
	 * for restore. In order to use the same API across both
	 * operations, we grab the file pointer and use it instead 
	 * of the descriptor.
	 */
	error = fget_read(curthread, fd, &cap_read_rights, &fp);
	if (error != 0) {
	    kern_close(curthread, fd);
	    return error;
	}

	/* Close the file. We can use the file pointer from here on. */
	kern_close(curthread, fd);

	/* Create the channel to be used. */
	chan.type = SLS_FILE;
	chan.fp = fp;
	chan.offset = 0;

	/* Export the results to the output variable. */
	*chanp = chan;

	return 0;
}

/* Create a SLOS-backed SLS channel. */
static int
slschan_initslos(uint64_t id, struct sls_channel *chanp)
{
	struct slos_node *vp;
	struct sls_channel chan;
	int error;

	/* Create an inode in the SLOS, if it doesn't already exist. */
	error = slos_icreate(&slos, id, S_IFREG);
	if (error != 0 && error != EINVAL) {
	    return error;
	}

	/* Open the inode, which due to the above call certainly exists. */
	vp = slos_iopen(&slos, id);
	if (vp == NULL)
	    return EIO;

	/* The dump itself. */
	chan.type = SLS_OSD;
	chan.vp = vp;
	chan.offset = 0;

	/* Export the results to the output variable. */
	*chanp = chan;

	return 0;
}

/* 
 * Create a channel from the specified backend. The backend can 
 * be thought of as a configuration for the process, from which
 * the active component, the channel, is constructed.
 */
int
slschan_init(struct sls_backend *sbak, struct sls_channel *chan)
{
	switch (sbak->bak_target) {
	case SLS_FILE:
	    return slschan_initfile(sbak->bak_name, chan);
	    break;

	case SLS_OSD:
	    return slschan_initslos(sbak->bak_id, chan);
	    break;

	default:
	    return EINVAL;
	}
}

/* Clean up the specified channel. */
void
slschan_fini(struct sls_channel *chan)
{
	switch (chan->type) {
	case SLS_FILE:
	    fdrop(chan->fp, curthread);
	    break;

	case SLS_OSD:
	    slos_iclose(&slos, chan->vp);
	    break;

	default:
	    return;
	}
}
