
#ifndef _SLS_DEBUG_COMMON_H_
#define _SLS_DEBUG_COMMON_H_

#include <sys/cdefs.h>
#include <sys/ktr.h>

#ifdef KTR
#define DEBUG(fmt) do {				    \
    CTR1(KTR_THIS_MODULE, "%s:"__XSTRING(__LINE__)" " fmt, __func__);			    \
    } while (0) 

#define DEBUG1(fmt, ...) do {			    \
    CTR2(KTR_THIS_MODULE, "%s:"__XSTRING(__LINE__)" " fmt, __func__, ##__VA_ARGS__);	    \
    } while (0) 

#define DEBUG2(fmt, ...) do {			    \
    CTR3(KTR_THIS_MODULE, "%s:"__XSTRING(__LINE__)" " fmt, __func__, ##__VA_ARGS__);	     \
    } while (0) 

#define DEBUG3(fmt, ...) do {			    \
    CTR4(KTR_THIS_MODULE, "%s:"__XSTRING(__LINE__)" " fmt, __func__, ##__VA_ARGS__);	    \
    } while (0) 

#define DEBUG4(fmt, ...) do {			    \
    CTR5(KTR_THIS_MODULE, "%s:"__XSTRING(__LINE__)" " fmt, __func__, ##__VA_ARGS__);	    \
    } while (0) 

#define DEBUG5(fmt, ...) do {			    \
    CTR6(KTR_THIS_MODULE, "%s:"__XSTRING(__LINE__)" " fmt, __func__, ##__VA_ARGS__);	    \
    } while (0) 

#else

#define DEBUG(fmt, ...) ((void)(0));
#define DEBUG1(fmt, ...) ((void)(0));
#define DEBUG2(fmt, ...) ((void)(0));
#define DEBUG3(fmt, ...) ((void)(0));
#define DEBUG4(fmt, ...) ((void)(0));
#define DEBUG5(fmt, ...) ((void)(0));

#endif // KTR

#endif /* _SLS_DEBUG_COMMON_H_ */

