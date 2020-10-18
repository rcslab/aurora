#ifndef _SLSTABLE_H_
#define _SLSTABLE_H_

#include <sys/malloc.h>

#include <vm/uma.h>
#include <vm/vm.h>
#include <vm/vm_map.h>

#include <sls_data.h>
#include <slos.h>

#include "sls_internal.h"
#include "sls_kv.h"

struct slos_node;


/* A run of contiguous pages for a VM object */
struct slspagerun {
    uint64_t	    idx;    /* The starting page offset in the object */
    uint64_t	    len;    /* The length of the page run in bytes */
    void		    *data;  /* The data for the pages */
    LIST_ENTRY(slspagerun) next;   /* The next run for the object */
};

/* A linked list of page runs */
LIST_HEAD(slsdata, slspagerun);	

/*
 * Read from and write to the SLOS. 
 */
int sls_read_slos(struct slspart *slsp, struct slskv_table **metatablep,
	struct slskv_table **datatablep);

int sls_write_slos(uint64_t oid, struct slsckpt_data *sckpt_data);

/* Zone for the pageruns. */
extern uma_zone_t slspagerun_zone;

struct sls_record *sls_getrecord(struct sbuf *sb, uint64_t slsid, uint64_t type);
void sls_record_destroy(struct sls_record *rec);
void sls_free_rectable(struct slskv_table *rectable);

extern unsigned int sls_async_slos;
extern unsigned int sls_sync_slos;

#ifdef SLS_TEST
int slstable_test(void);
#endif /* SLS_TEST */

#endif /* _SLSTABLE_H_ */
