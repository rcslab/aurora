#ifndef _SLS_DATA_H_
#define _SLS_DATA_H_

#include <sys/param.h>

#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/socket.h>
#include <sys/tty.h>
#include <sys/ttycom.h>
#include <sys/un.h>

#include <machine/param.h>
#include <machine/pcb.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_map.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#define SLSPROC_ID 0x736c7301
struct slsproc {
	uint64_t	magic;	    /* Magic value */
	uint64_t	slsid;	    /* Unique object ID */
	size_t		nthreads;   /* Threads in a process */
	pid_t		pid;	    /* PID of the process */
	pid_t		pgid;	    /* ID of the process group */
	uint64_t	pptr;	    /* Unique object ID of the parent proc */
	uint64_t	pgrpwait;   /* Should we wait for the process group
				       to be rebuilt? */
	pid_t		sid;	    /* Session ID */
	struct sigacts sigacts;	    /* Signal handling info */
	char	    name[MAXCOMLEN + 1]; /* Name of the process */
};

#define SLSTHREAD_ID 0x736c7302
struct slsthread {
	uint64_t magic;
	uint64_t slsid;
	struct reg regs;
	struct fpreg fpregs;
	lwpid_t tid;
	sigset_t sigmask;
	sigset_t oldsigmask;
	uint64_t fs_base;
	uint64_t tf_err;
};

/* State of the vmspace, but also of its vm_map */
#define SLSVMSPACE_ID 0x736c730a
struct slsvmspace {
	uint64_t magic;
	uint64_t slsid;
	/* State of the vmspace object */
	segsz_t vm_swrss;
	segsz_t vm_tsize;
	segsz_t vm_dsize;
	segsz_t vm_ssize;
	caddr_t vm_taddr;
	caddr_t vm_daddr;
	caddr_t vm_maxsaddr;
	int nentries;
	int has_shm;
};

#define SLSSESSION_ID 0x736c730b
struct slssession {
	uint64_t magic;
	uint64_t slsid;
	uint64_t tty;
	uint64_t sid;
	uint64_t leader;
};

#define SLSVMOBJECT_ID 0x7abc7303
struct slsvmobject {
	uint64_t magic;
	uint64_t slsid;
	vm_pindex_t size;
	enum obj_type type;
	struct sbuf *path; 
	vm_object_t id;
	/* Used for objects that are shadows of others */
	vm_object_t backer;
	vm_ooffset_t backer_off;

	/* XXX Bookkeeping for swapped out pages? */
};

#define SLSVMENTRY_ID 0x736c7304
/* State of a vm_map_entry and its backing object */
struct slsvmentry {
	uint64_t magic;
	uint64_t slsid;
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

#define SLSFILE_ID 0x736c7234
#define SLSSTRING_ID 0x72626f72
struct slsfile {
	uint64_t magic;
	uint64_t slsid;

	short type;
	u_int flag;

	off_t offset;
	uint64_t backer;

	/* 
	* Let's not bother with this flag from the filedescent struct.
	* It's only about autoclosing on exec, and we don't really care right now 
	*/
	/* uint8_t fde_flags; */
};

#define SLSFILEDESC_ID 0x736c7233
struct slsfiledesc {
	uint64_t magic;
	uint64_t slsid;

	struct sbuf *cdir;
	struct sbuf *rdir;
	/* TODO jdir */

	u_short fd_cmask;
	int fd_holdcnt;

	/* XXX Get any buffered data */
};

#define SLSPIPE_ID  0x736c7499
struct slspipe {
	uint64_t    magic;	/* Magic value */
	uint64_t    slsid;	/* Unique SLS ID */
	uint64_t    iswriteend;	/* Is this the write end? */
	uint64_t    peer;	/* The SLS ID of the other end */
	/* XXX Get data */
};

#define SLSKQUEUE_ID  0x736c7265
struct slskqueue {
	uint64_t    magic;
	uint64_t    slsid;	/* Unique SLS ID */
	uint64_t    numevents;
	/* XXX In-flight knotes */
};

#define SLSKEVENT_ID  0x736c7115
struct slskevent {
	uint64_t    magic;
	uint64_t    slsid;	/* Unique SLS ID */
	int32_t	    status;
	int64_t	    ident;
	int16_t	    filter;
	int32_t	    flags;
	int32_t	    fflags;
	int64_t	    data;
};

#define SLSSOCKET_ID  0x736c7268
struct slssock {
	/* SLS Object options */
	uint64_t    magic;
	uint64_t    slsid;	/* Unique SLS ID */
	uint64_t    state ;	/* Flags */

	/* Socket-wide options */
	int16_t	    family;
	int16_t	    type;
	int16_t	    proto;
	uint32_t    options;

	/* UNIX or INET addresses */
	union {
	    struct sockaddr_un un;
	    struct sockaddr_in in;
	};

	uint64_t    unpeer;	/* UNIX socket peer */
	uint64_t    bound;	/* Is the socket bound? */	

	/* XXX Maybe encapsulate INET state? */
	struct in_conninfo in_conninfo;
	uint64_t    backlog;
};


#define SLSPTS_ID  0x736c7269
struct slspts {
	uint64_t magic;
	uint64_t slsid;

	/* 
	 * Find out if it's the master or the
	 * slave side. If it's the slave side
	 * we only need the peer's ID.
	 */
	uint64_t ismaster;
	uint64_t peerid;
	uint64_t flags;

	int drainwait;
	struct termios termios;
	struct winsize winsize;
	unsigned int column;
	unsigned int writepos;
	struct termios termios_init_in;
	struct termios termios_lock_in;
	struct termios termios_init_out;
	struct termios termios_lock_out;
	/* XXX Get any buffered data */
};

#define SLSSYSVSHM_ID  0x736c7232
struct slssysvshm {
	uint64_t magic;
	uint64_t slsid;
	uint64_t shm_segsz;
	mode_t mode;
	key_t key;
	int segnum;
};

#define SLSPOSIXSHM_ID  0x736c7232
struct slsposixshm {
	uint64_t    magic;
	uint64_t    slsid;
	mode_t	    mode;
	uint64_t    object;
	uint64_t    is_anon;
	/* XXX Restore only, hacky. Refactor. */
	struct sbuf *sb;

	/* XXX Rangelocks? */
};



#endif /* _SLS_DATA_H_ */
