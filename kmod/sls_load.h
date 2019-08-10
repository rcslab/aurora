#ifndef _SLS_RESTORE_H_
#define _SLS_RESTORE_H_

#include "sls_channel.h"
#include "sls_process.h"

struct dump_page {
	vm_ooffset_t vaddr;
	void *data;
	LIST_ENTRY(dump_page) next;
};

LIST_HEAD(page_list, dump_page);

struct sls_pagetable {
    struct page_list *pages;		
    u_long hashmask;		
};

int sls_load_thread(struct thread_info *thread_info, struct sls_channel *chan);
int sls_load_proc(struct proc_info *proc_info, struct sls_channel *chan);
int sls_load_file(struct file_info *file, struct sls_channel *chan);
int sls_load_filedesc(struct filedesc_info *filedesc, struct sls_channel *chan);
int sls_load_vmobject(struct vm_object_info *obj, struct sls_channel *chan);
int sls_load_vmentry(struct vm_map_entry_info *entry, struct sls_channel *chan);
int sls_load_memory(struct memckpt_info *memory, struct sls_channel *chan);
int sls_load_path(struct sbuf **sbp, struct sls_channel *chan); 

int sls_load_ptable(struct sls_pagetable *ptable, struct sls_channel *chan);

#endif /* _SLS_RESTORE_H_ */
