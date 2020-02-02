#include <slsfs.h>
#include <slos_btree.h>
#include <slos_record.h>

#include <slsfs_node.h>

int
slsfs_key_insert(struct slos_node *svp, uint64_t key, struct slos_recentry val) 
{
	struct btree *tree;
	struct slos_record *rec;
	int error = 0;

	/*
	 * We use the recentry data structure as currently the record btrees 
	 * store recentrys which are represent their disk location and offset 
	 * and len in the block. 
	 */
	KASSERT(val.len == IOSIZE(svp), ("Currently we only accept sizes of blksize"));
	error = slos_firstrec(svp, &rec);
	if (error && error == EINVAL) {
		size_t rno;
		error = slos_rcreate(svp, SLOSREC_DATA, &rno);
		if (error) {
			return (error);
		}

		error = slos_firstrec(svp, &rec);
		if (error) {
			return (error);
		}
	} else if (error && error == EIO) {
		DBUG("Problem reading first data record\n");
		return (error);
	}

	tree = btree_init(svp->sn_slos, rec->rec_data.offset, ALLOCMAIN);
	error = btree_insert(tree, key, &val);
	btree_destroy(tree);

	return (error);
}

int 
slsfs_key_remove(struct slos_node *svp, uint64_t key)
{
	struct btree *tree;
	struct slos_record *rec;
	int error = 0;

	error = slos_firstrec(svp, &rec);
	if (error) {
	    return (error);
	}

	tree = btree_init(svp->sn_slos, rec->rec_data.offset, ALLOCMAIN);
	error = btree_delete(tree, key);
	btree_destroy(tree);

	return (error);
}
