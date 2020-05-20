#ifndef _SLSTABLE_TEST_H_
#define _SLSTABLE_TEST_H_

#include <sys/param.h>

#define INFO_SIZE   (20)	/* Size of the data of the test structs. */
#define VMOBJ_SIZE  (1024)	/* Extent of the test's VM objects, in pages */
#define VNODE_ID    (0xabcdef)	/* ID of the testing vnode in the SLOS */
#define DATA_INFOS  (32)	/* Number of data structs */
#define META_INFOS  (128)	/* Number of meta structs */
#define DATA_SIZE   (512) /* Average number of resident pages per object */

/* 
 * Struct that plays the role of vm_object_info structs, since it holds
 * data. For this struct the slsid matters, since it must point to 
 * a valid VM object. 
 *
 * The sbid field is a pointer to the sb object in which the 
 * data info is held; this kind of information is only needed
 * for this test, where we want to be able to draw 1-1 relations 
 * between the sbufs for original objects and the restored objects.
 */
#define DATA_MAGIC 0xbadcafe
typedef struct data_info {
    uint64_t magic;
    uint64_t slsid; /* Unique ID, doubles as a pointer. */
    uint64_t size;  /* The size of the object. */
    uint8_t  data[INFO_SIZE];
} data_info;

/*
 * Struct that plays the role of all other structs; the only data it has
 * is what it holds in itself. In this case the slsid does not matter,
 * since there is no original object from which the struct is derived.
 *
 */
#define META_MAGIC 0xaddbeef
typedef data_info meta_info;

/* Types for the meta and data info records on the disk */
#define SLOSREC_TESTDATA	(0x1337c0de)
#define SLOSREC_TESTMETA	(0x11111111)

#endif /* _SLSTABLE_TEST_H_ */
