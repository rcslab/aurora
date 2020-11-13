#include <sys/param.h>
#include <sys/endian.h>
#include <sys/queue.h>

#include <machine/param.h>

#include <sys/cdefs.h>
#include <sys/conf.h>
#include <sys/domain.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/selinfo.h>
#include <sys/shm.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/tty.h>
#include <sys/unistd.h>
#include <sys/un.h>
#include <sys/unpcb.h>
#include <sys/vnode.h>

/* XXX Pipe has to be after selinfo */
#include <sys/pipe.h>

/* 
 * XXX eventvar should include more headers,
 * it can't be placed alphabetically.
 */
#include <sys/eventvar.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <slos.h>
#include <sls_data.h>

#include "sls_file.h"
#include "sls_internal.h"
#include "debug.h"

int
slsckpt_posixshm(struct shmfd *shmfd, struct sbuf *sb)
{
	struct slsposixshm slsposixshm;
	int error;

	slsposixshm.slsid = (uint64_t) shmfd;
	slsposixshm.magic = SLSPOSIXSHM_ID;
	slsposixshm.mode = shmfd->shm_mode;
	slsposixshm.object = (uint64_t) shmfd->shm_object->objid;
	slsposixshm.is_named = (shmfd->shm_path != NULL); 

	/* Write down the path, if it exists. */
	if (shmfd->shm_path != NULL)
		strlcpy(slsposixshm.path, shmfd->shm_path, PATH_MAX);

	/* 
	 * While the shmfd is unique for the shared memory, and so
	 * might be pointed to by multiple open files, the size of 
	 * the metadata is small enough that we can checkpoint 
	 * multiple times, and restore only once. 
	 */
	error = sbuf_bcat(sb, &slsposixshm, sizeof(slsposixshm));
	if (error != 0) 
		return (error);

	return (0);
}

int
slsrest_posixshm(struct slsposixshm *info, struct slskv_table *objtable, int *fdp)
{
	vm_object_t oldobj, obj;
	struct shmfd *shmfd;
	struct file *fp;
	char *path;
	int error;
	int fd;

	/* First and foremost, go fetch the object backing the shared memory. */
	error = slskv_find(objtable, info->object, (uintptr_t *) &obj);
	if (error != 0)
		return (error);

	path = (info->is_named) ? info->path : SHM_ANON;
	DEBUG1("Restoring shared memory with path %p",
	    path == SHM_ANON ? path : "(anon)");

	/* First try to create the shared memory mapping. */
	error = kern_shm_open(curthread, path, UIO_SYSSPACE,
	    O_RDWR | O_CREAT | O_EXCL, info->mode, NULL);
	if (error != 0) {
		DEBUG("Failed to create new shared memory segment");

		/* Maybe it's already created then? */
		error = kern_shm_open(curthread, path, UIO_SYSSPACE,
		    O_RDWR, info->mode, NULL);
		if (error != 0)
			return (error);

		DEBUG("Shared memory segment already created");

		/* It was - return the fd and let the main code take care of the rest. */
		*fdp = curthread->td_retval[0];
		return (0);
	}

	/* Otherwise we just created it. */
	fd = curthread->td_retval[0];

	/* Change the shared memory segment to point to the restored data. */
	fp = FDTOFP(curproc, fd);
	shmfd = (struct shmfd *) fp->f_data;

	DEBUG1("Swapped object %p into shared memory segment", obj);
	vm_object_reference(obj);

	oldobj = shmfd->shm_object;
	shmfd->shm_object = obj;

	vm_object_deallocate(oldobj);

	*fdp = fd;

	return (0);
}
