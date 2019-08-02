#ifndef _SLOS_RECORD_H_
#define _SLOS_RECORD_H_

#include <sys/param.h>

int slos_rcreate(struct slos_vnode *vp, uint64_t rtype, uint64_t *rnop);
/* XXX Implement */
uint64_t slos_rclone(struct slos_vnode *vp, uint64_t rno);
int slos_rremove(struct slos_vnode *vp, uint64_t rno);
int slos_rread(struct slos_vnode *vp, uint64_t rno, struct uio *auio);
int slos_rwrite(struct slos_vnode *vp, uint64_t rno, struct uio *auio);
/* XXX Implement */
int slos_rtrim(struct slos_vnode *vp, uint64_t rno, struct uio *auio);
/* XXX Find proper signature */
void *slos_riter(struct slos_vnode *vp, uint64_t rno, struct uio *auio);

/* XXX Find proper signature */
uint64_t slos_rstat(struct slos_vnode *vp);

#ifdef SLOS_TESTS

int slos_test_record(void);

#endif /* SLOS_TESTS */

#endif /* _SLOS_RECORD_H_ */
