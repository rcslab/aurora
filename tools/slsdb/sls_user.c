#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../kmod/dump.h"
#include "sls_user.h"

struct slspage *pagelist;

/* If a memory allocation fails, we just bail */
static void *
read_array(size_t numelements, size_t element_size, FILE *fp)
{
    void *array;
    size_t total_size;
    size_t ret;

    total_size = numelements * element_size;

    array = malloc(total_size);
    assert(array != NULL);

    ret = fread(array, element_size, numelements, fp);
    if (ret != numelements) {
        fprintf(stderr, "Error: Array not read\n");
        exit(0);
    }

    return array;
}

static char *
read_string(size_t size, FILE *fp)
{
    char *str;
    int ret;

    str = malloc(size);
    assert(str != NULL);

    ret = fread(str, size, 1, fp);
    if (ret != 1) {
        fprintf(stderr, "Error: String not read\n");
        exit(0);
    }

    return str;
}

struct dump *
sls_load_dump(FILE *fp)
{
    struct dump *dump;
    struct thread_info *threads;
    struct file_info *files;
    struct vm_map_entry_info *entries;
    struct vm_object_info *object;
    size_t numthreads, numfiles, numentries;
    struct slspage *page, *page_tail;
    uint64_t vaddr; 
    size_t ret;
    int i, j;

    dump = malloc(sizeof(*dump));
    assert(dump != NULL);

    ret = fread(dump, sizeof(*dump), 1, fp);
    if (ret != 1) {
        fprintf(stderr, "Error: dump not read\n");
        exit(0);
    }

    /* 
     * XXX When an assertion fails, it will be be
     * very nice to show the dump read up to that 
     * point, that way we can pinpoint the mismathc
     * that caused the assertion fail.
     */
    assert(dump->magic == SLS_DUMP_MAGIC);
    assert(dump->proc.magic == SLS_PROC_INFO_MAGIC);
    assert(dump->memory.vmspace.magic == SLS_VMSPACE_INFO_MAGIC);
    assert(dump->filedesc.magic == SLS_FILEDESC_INFO_MAGIC);

    numthreads = dump->proc.nthreads;
    numentries = dump->memory.vmspace.nentries;
    numfiles = dump->filedesc.num_files;
    
    threads = read_array(numthreads, sizeof(*threads), fp);
    files = read_array(numfiles, sizeof(*files), fp);
    entries = read_array(numentries, sizeof(*entries), fp);

    dump->threads = threads;
    dump->filedesc.infos = files;
    dump->memory.entries = entries;

    for (i = 0; i < numthreads; i++) 
        assert(threads[i].magic == SLS_THREAD_INFO_MAGIC); 

    for (i = 0; i < numfiles; i++) 
        assert(files[i].magic == SLS_FILE_INFO_MAGIC); 

    for (i = 0; i < numentries; i++) 
        assert(entries[i].magic == SLS_ENTRY_INFO_MAGIC); 

    for (i = 0; i < numentries; i++) {
	if (entries[i].obj_info == NULL)
	    continue;
	
	object = malloc(sizeof(*object));
	ret = fread(object, sizeof(*object), 1, fp);
	assert(ret == 1);
        assert(object->magic == SLS_OBJECT_INFO_MAGIC); 

	entries[i].obj_info = object;
    }

    /* XXX Unify string reading by having a central "database" */
    dump->filedesc.cdir = read_string(dump->filedesc.cdir_len, fp);
    dump->filedesc.rdir = read_string(dump->filedesc.rdir_len, fp);

    for(i = 0; i < numfiles; i++) {
        files[i].filename = read_string(files[i].filename_len, fp);
    }
         
    for(i = 0; i < numentries; i++) {
	object = entries[i].obj_info;
	if (object == NULL)
	    continue;

        if (object->filename_len == 0)
            continue;

        object->filename = read_string(object->filename_len, fp);
    }
    return dump;

    pagelist = NULL; 
    for(;;) {

	ret = fscanf(fp, "%lu\n", &vaddr);
	if (vaddr == ULONG_MAX)
	    break;

	page = malloc(sizeof(*page));
	assert(page != 0);

	page->vaddr = vaddr;
	
	/*
	if (ret != 1) {
	    printf("fscanf failed at entry %d, page %d with %lu\n", i, j, ret);
	    exit(0);
	}

	*/
	ret = fread(&page->data, PAGE_SIZE, 1, fp);
	if (ret != 1) {
	    printf("Reading page with address %p failed with %lu\n", (void *) page->vaddr, ret);
	    exit(0);
	}

	if (pagelist == NULL)
	    pagelist = page;
	else
	    page_tail->next = page; 

	page_tail = page; 
	page->next = NULL;

    }

    return dump;
}


