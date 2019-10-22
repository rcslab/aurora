#ifndef _SLOS_RECORD_H_
#define _SLOS_RECORD_H_

#include <sys/param.h>
#include "slosmm.h"

int slos_rcreate(struct slos_node *vp, uint64_t rtype, uint64_t *rnop);
/* XXX Implement */
uint64_t slos_rclone(struct slos_node *vp, uint64_t rno);
int slos_rremove(struct slos_node *vp, uint64_t rno);
int slos_rread(struct slos_node *vp, uint64_t rno, struct uio *auio);
int slos_rwrite(struct slos_node *vp, uint64_t rno, struct uio *auio);
/* XXX Implement */
int slos_rtrim(struct slos_node *vp, uint64_t rno, struct uio *auio);

/* Flags for slos_seek. */
#define SREC_SEEKLEFT	0b001
#define SREC_SEEKRIGHT	0b010
#define SREC_SEEKHOLE	0b100

/* Special value for seeklenp when we don't find an extent/hole. */
#define SREC_SEEKEOF	0
#define SLOS_RECORD_FOREACH(slsvp, record, tmp) \
    for(tmp = NULL, record = slos_firstrec(slsvp); record != NULL;  \
	    tmp = record, record = slos_nextrec(slsvp, record->rec_num), free(tmp, M_SLOS)) \

int slos_rseek(struct slos_node *vp, uint64_t rno, uint64_t offset, 
	int flags, uint64_t *seekoffp, uint64_t *seeklenp);

/* Stat structure for individual records. */
struct slos_rstat {
	uint64_t type;	/* The type of the record */
	uint64_t len;	/* The length of the record */
};

int slos_rstat(struct slos_node *vp, uint64_t rno, struct slos_rstat *stat);

/* Records btree iterators */
int slos_firstrno(struct slos_node *vp, uint64_t *rnop);
int slos_lastrno(struct slos_node *vp, uint64_t *rnop);
int slos_prevrno(struct slos_node *vp, uint64_t *rnop);
int slos_nextrno(struct slos_node *vp, uint64_t *rnop);
int slos_firstrno_typed(struct slos_node *vp, uint64_t rtype, uint64_t *rnop);
int slos_lastrno_typed(struct slos_node *vp, uint64_t rtype, uint64_t *rnop);

struct slos_record *slos_prevrec(struct slos_node *vp, uint64_t rno);
struct slos_record *slos_nextrec(struct slos_node *vp, uint64_t rno);
struct slos_record *slos_firstrec(struct slos_node *vp);

#ifdef SLOS_TESTS

int slos_test_record(void);

#endif /* SLOS_TESTS */

#endif /* _SLOS_RECORD_H_ */
