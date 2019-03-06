#ifndef _FD_H_
#define _FD_H_

#include <sys/types.h>

#include <sys/file.h>
#include <sys/filedesc.h>

#define SLS_FILE_INFO_MAGIC 0x736c7234
struct file_info {
	/* 
	* Let's hope the private data doesn't get used
	* for regular files, because we're not saving it.
	*/
	/*
	* XXX Merge with mmap stuff, there's 
	* duplication when doing vnode to filename conversions and back.
	*/
	int fd;
	char *filename;
	size_t filename_len;

	short type;
	u_int flag;

	off_t offset;


	/* 
	* Let's not bother with this flag from the filedescent struct.
	* It's only about autoclosing on exec, and we don't really care right now 
	*/
	/* uint8_t fde_flags; */
	int magic;
};

#define SLS_FILEDESC_INFO_MAGIC 0x736c7233
struct filedesc_info {
	char *cdir;
	size_t cdir_len;

	char *rdir;
	size_t rdir_len;
	/* TODO jdir */

	u_short fd_cmask;
	/* XXX Should these go to 1? */
	int fd_refcnt;
	int fd_holdcnt;
	/* TODO: kqueue list? */

	int num_files;

	struct file_info *infos;
	int magic;
};

int fd_checkpoint(struct filedesc *filedesc, struct filedesc_info *filedesc_info);
int fd_restore(struct proc *p, struct filedesc_info *info);

#endif
