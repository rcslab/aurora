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
int sls_read_slos(uint64_t oid, struct slskv_table **metatablep,
	struct slskv_table **datatablep);

int sls_write_slos(uint64_t oid, struct slsckpt_data *sckpt_data);

/* Zone for the pageruns. */
extern uma_zone_t slspagerun_zone;

#ifdef SLS_TEST
int slstable_test(void);
#endif /* SLS_TEST */

#endif /* _SLSTABLE_H_ */
