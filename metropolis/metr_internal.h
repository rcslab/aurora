#ifndef _METR_INTERNAL_
#define _METR_INTERNAL_

struct metr_metadata {
	struct mtx metrm_mtx;	       /* Structure mutex. */
	struct cv metrm_exitcv;	       /* Structure CV. */
	LIST_HEAD(, proc) metrm_plist; /* List of Metropolis processes */
	uint64_t metrm_procs;	       /* Processes registered */
	struct cdev *metrm_cdev;       /* Metropolis API device */
};

#define METR_ASSERT_LOCKED() (mtx_assert(&metrm.metrm_mtx, MA_OWNED))
#define METR_ASSERT_UNLOCKED() (mtx_assert(&metrm.metrm_mtx, MA_NOTOWNED))
#define METR_LOCK() mtx_lock(&metrm.metrm_mtx)
#define METR_UNLOCK() mtx_unlock(&metrm.metrm_mtx)
#define METR_EXITING() (metrm.metrm_exiting != 0)

#endif /* _METR_INTERNAL_ */
