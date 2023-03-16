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
#include "sls_prefault.h"

/*
 * Read from and write to the SLOS.
 */
int sls_read_slos(struct slspart *slsp, struct slsckpt_data **sckptp,
    struct slskv_table *objtable);

int sls_readdata_prefault(
    struct vnode *vp, vm_object_t obj, struct sls_prefault *slspre);
int sls_write_slos(uint64_t oid, struct slsckpt_data *sckpt);
int sls_write_slos_dataregion(struct slsckpt_data *sckpt);

struct sls_record *sls_getrecord_empty(uint64_t slsid, uint64_t type);
struct sls_record *sls_getrecord(
    struct sbuf *sb, uint64_t slsid, uint64_t type);
int sls_record_seal(struct sls_record *rec);
void sls_record_destroy(struct sls_record *rec);
void sls_free_rectable(struct slskv_table *rectable);

extern unsigned int sls_async_slos;

int slstable_init(void);
void slstable_fini(void);

int sls_import_ssparts(void);
int sls_export_ssparts(void);

int sls_readmeta(char *buf, size_t buflen, struct slskv_table *table);

int sls_read_file(struct slspart *slsp, struct slsckpt_data **sckpt,
    struct slskv_table *objtable);
int sls_write_file(struct slspart *slsp, struct slsckpt_data *sckpt);
int sls_write_socket(struct slspart *slsp, struct slsckpt_data *sckpt);

#endif /* _SLSTABLE_H_ */
