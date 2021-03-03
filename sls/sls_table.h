#ifndef _SLSTABLE_H_
#define _SLSTABLE_H_

#include <sys/malloc.h>

#include <vm/vm.h>
#include <vm/uma.h>
#include <vm/vm_map.h>

#include <slos.h>
#include <sls_data.h>

#include "sls_internal.h"
#include "sls_kv.h"

/*
 * Read from and write to the SLOS.
 */
int sls_read_slos(struct slspart *slsp, struct slskv_table **rectable,
    struct slskv_table *objtable);

int sls_write_slos(uint64_t oid, struct slsckpt_data *sckpt_data);
int sls_write_slos_dataregion(struct slsckpt_data *sckpt_data);

struct sls_record *sls_getrecord(
    struct sbuf *sb, uint64_t slsid, uint64_t type);
void sls_record_destroy(struct sls_record *rec);
void sls_free_rectable(struct slskv_table *rectable);

extern unsigned int sls_async_slos;

#endif /* _SLSTABLE_H_ */
