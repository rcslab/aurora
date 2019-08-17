#ifndef _SLS_DATA_H_
#define _SLS_DATA_H_

#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/sbuf.h>

#include <machine/param.h>
#include <machine/pcb.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_map.h>

#define SLS_PROC_INFO_MAGIC 0x736c7301
struct proc_info {
	int magic;
	size_t nthreads;
	pid_t pid;
	/* All fields valid except for the mutex */
	struct sigacts sigacts;
};

#define SLS_THREAD_INFO_MAGIC 0x736c7302
struct thread_info {
	int magic;
	struct reg regs;
	struct fpreg fpregs;
	lwpid_t tid;
	sigset_t sigmask;
	sigset_t oldsigmask;
	uint64_t fs_base;
};

/* State of the vmspace, but also of its vm_map */
struct vmspace_info {
	int magic;
	/* State of the vmspace object */
	segsz_t vm_swrss;
	segsz_t vm_tsize;
	segsz_t vm_dsize;
	segsz_t vm_ssize;
	caddr_t vm_taddr;
	caddr_t vm_daddr;
	caddr_t vm_maxsaddr;
	int nentries;
	/*  
	 * Needed to know how many index-page pairs
	 * to read from the file 
	 */
};

#define SLS_OBJECT_INFO_MAGIC 0x7aaa7303
#define SLS_OBJECTS_END 0x7abab34
struct vm_object_info {
	vm_pindex_t size;
    
	enum obj_type type;

	struct sbuf *path; 

	vm_object_t id;

	/* Used for objects that are shadows of others */
	vm_object_t backer;
	vm_ooffset_t backer_off;

	/* XXX Bookkeeping for swapped out pages? */
	int magic;
};

#define SLS_ENTRY_INFO_MAGIC 0x736c7304
/* State of a vm_map_entry and its backing object */
struct vm_map_entry_info {
	int magic;
	/* State of the map entry */
	vm_offset_t start;
	vm_offset_t end;
	vm_ooffset_t offset;
	vm_eflags_t eflags;
	vm_prot_t protection;
	vm_prot_t max_protection;
	vm_object_t obj;
	vm_inherit_t inheritance;
	enum obj_type type;
};


#define SLS_MEMCKPT_INFO_MAGIC 0x736c730a
struct memckpt_info {
	/* 
	 * XXX Actually use the magic, we aren't actually reading/
	 * writing it right now 
	 */
	int magic;
	struct vmspace_info vmspace;
	struct vm_map_entry_info *entries;
};

#define SLS_FILE_INFO_MAGIC 0x736c7234
#define SLS_STRING_MAGIC 0x72626f72
#define SLS_FILES_END INT_MAX
struct file_info {
	int fd;

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

	struct sbuf *cdir;
	struct sbuf *rdir;
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

#define SLS_PIPE_INFO_MAGIC  0x736c7499
struct pipe_info {
	uint64_t    iswriteend;	/* Is this the write end? */
	uint64_t    otherend;	/* The fd of the other end */
	uint64_t    needrest;	/* Should we restore this? */
	uint64_t    onlyend;	/* Are we the only end? */
	uint64_t    magic;	/* Magic value */
};

#define SLS_KQUEUE_INFO_MAGIC  0x736c7265
struct kqueue_info {
	uint64_t    numevents;
	uint64_t    magic;
};

#define SLS_KEVENT_INFO_MAGIC  0x736c7115
struct kevent_info {
	int32_t	    status;
	int64_t	    ident;
	int16_t	    filter;
	int32_t	    flags;
	int32_t	    fflags;
	int64_t	    data;
	uint64_t    magic;
};

#define SLS_SOCKET_INFO_MAGIC  0x736c7268
struct sock_info {
	/* Socket-wide options */
	int16_t	    family;
	int16_t	    type;
	int16_t	    proto;
	uint32_t    options;

	/* Fields saved in VPS that do not make much sense to restore right now. */
	/*
	uint32_t    qlimit;
	uint32_t    qstate;
	int16_t	    state;
	int32_t	    qlen;
	int16_t	    incqlen;
	*/

	/* Internet protocol-related options */
	uint8_t	    vflag;
	uint8_t	    ip_p;
	uint8_t	    have_ppcb;
	int32_t	    flags;
	int32_t	    flags2;

	struct {
		uint8_t inc_flags;
		uint8_t inc_len;
		uint16_t inc_fibnum;

		uint8_t ie_ufaddr[0x10];

		uint8_t ie_uladdr[0x10];

		uint16_t ie_fport;
		uint16_t ie_lport;
	} inp_inc;
};

#endif /* _SLS_DATA_H_ */
