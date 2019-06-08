#ifndef _SLS_RESTORE_H_
#define _SLS_RESTORE_H_

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

int sls_load_thread(struct thread_info *thread_info, struct file *fp);
int sls_load_proc(struct proc_info *proc_info, struct file *fp);
int sls_load_file(struct file_info *file, struct file *fp);
int sls_load_filedesc(struct filedesc_info *filedesc, struct file *fp);
int sls_load_ptable(struct sls_pagetable *ptable, struct file *fp);
int sls_load_vmobject(struct vm_object_info *obj, struct file *fp);
int sls_load_vmentry(struct vm_map_entry_info *entry, struct file *fp);
int sls_load_memory(struct memckpt_info *memory, struct file *fp);

void addpage_noreplace(struct sls_pagetable *ptable, struct dump_page *dump_page);

#endif /* _SLS_RESTORE_H_ */
