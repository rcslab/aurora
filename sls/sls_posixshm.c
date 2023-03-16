#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/selinfo.h>
#include <sys/conf.h>
#include <sys/domain.h>
#include <sys/endian.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/shm.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/tty.h>
#include <sys/un.h>
#include <sys/unistd.h>
#include <sys/unpcb.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <sls_data.h>

#include "debug.h"
#include "sls_file.h"
#include "sls_internal.h"
#include "sls_vmobject.h"

static void
slsshm_restore_swapobj(struct file *fp, vm_object_t obj)
{
	struct shmfd *shmfd = (struct shmfd *)fp->f_data;
	vm_object_t oldobj;

	DEBUG1("Swapped object %p into shared memory segment", obj);
	vm_object_reference(obj);

	oldobj = shmfd->shm_object;
	shmfd->shm_object = obj;

	vm_object_deallocate(oldobj);
}

static int
slsshm_restore(void *slsbacker, struct slsfile *finfo,
    struct slsrest_data *restdata, struct file **fpp)
{
	struct slsposixshm *info = (struct slsposixshm *)slsbacker;
	vm_object_t obj;
	struct file *fp;
	char *path;
	int error;
	int fd;

	/* First and foremost, go fetch the object backing the shared memory. */
	error = slskv_find(restdata->objtable, info->object, (uintptr_t *)&obj);
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
		error = kern_shm_open(
		    curthread, path, UIO_SYSSPACE, O_RDWR, info->mode, NULL);
		if (error != 0)
			return (error);

		DEBUG("Shared memory segment already created");

		fd = curthread->td_retval[0];

		/* Yes, return the fd let the caller take care of the rest. */
		error = slsfile_extractfp(fd, &fp);
		if (error != 0) {
			slsfile_attemptclose(fd);
			return (error);
		}

		*fpp = fp;

		return (0);
	}

	/* Otherwise we just created it. */
	fd = curthread->td_retval[0];
	error = slsfile_extractfp(fd, &fp);
	if (error != 0) {
		slsfile_attemptclose(fd);
		return (error);
	}

	slsshm_restore_swapobj(fp, obj);

	*fpp = fp;

	return (0);
}

static int
slsshm_slsid(struct file *fp, uint64_t *slsidp)
{
	*slsidp = (uint64_t)(fp->f_data);

	return (0);
}

static int
slsshm_checkpoint(
    struct file *fp, struct sbuf *sb, struct slsckpt_data *sckpt_data)
{
	struct slsposixshm slsposixshm;
	struct shmfd *shmfd;
	int error;

	shmfd = (struct shmfd *)fp->f_data;

	slsposixshm.slsid = (uint64_t)shmfd;
	slsposixshm.magic = SLSPOSIXSHM_ID;
	slsposixshm.mode = shmfd->shm_mode;
	slsposixshm.object = (uint64_t)shmfd->shm_object->objid;
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

	/* Update the object pointer, possibly shadowing the object. */
	error = slsvmobj_checkpoint_shm(&shmfd->shm_object, sckpt_data);
	if (error != 0)
		return (error);

	return (0);
}

static bool
slsshm_supported(struct file *fp)
{
	return (true);
}

struct slsfile_ops slsshm_ops = {
	.slsfile_supported = slsshm_supported,
	.slsfile_slsid = slsshm_slsid,
	.slsfile_checkpoint = slsshm_checkpoint,
	.slsfile_restore = slsshm_restore,
};
