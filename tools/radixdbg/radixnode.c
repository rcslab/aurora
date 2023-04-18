#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <slos.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sysexits.h>
#include <unistd.h>

#include "radixdbg.h"
#include "radixnode.h"

static inline int
stree_localkey(uint64_t key, int depth)
{
	uint64_t movbits = (STREE_DEPTH - 1 - depth) * fls(SRDXMASK);

	return ((key >> movbits) & SRDXMASK);
}

void
srdx_retrieve(int fd, uint64_t lblkno, rdxnode *srdxp)
{
	rdxnode srdx;
	int ret;

	srdx = malloc(BLKSIZE);
	assert(srdx != NULL);

	ret = pread(fd, srdx, BLKSIZE, BLKSIZE * lblkno);
	if (ret < 0) {
		perror("read");
		exit(EX_DATAERR);
	}

	*srdxp = srdx;
}

void
srdx_release(rdxnode srdx)
{
	free(srdx);
}

static inline int
stree_localkey_increment(uint64_t *key, int depth)
{
	uint64_t movbits = (STREE_DEPTH - 1 - depth) * fls(SRDXMASK);

	if (*key == KEYMAX)
		return (EINVAL);

	/* Zero out the bits before the local key. */
	if (movbits > 0)
		*key = *key & ~((1ULL << movbits) - 1);
	/* Increment the local key. */
	*key = *key + (1ULL << movbits);
	return (0);
}

/*
 * Find the first valid value in the node with a key >= the one given.
 */
static bool
stree_siter_moveright(rdxnode *srdx, uint64_t *key, int depth)
{
	uint64_t localkey;
	diskblk_t localvalue;

	for (;;) {
		localkey = stree_localkey(*key, depth);
		/* Will trigger if the tree is empty. */
		assert(localkey < SRDXCAP);

		/* Go down if possible, otherwise go up. */
		localvalue = SRDXVAL(srdx, localkey);
		if (STREE_VALVALID(localvalue))
			return true;

		/* Reached the end of the node. */
		if (localkey + 1 == SRDXCAP)
			return false;

		stree_localkey_increment(key, depth);
	}
}

static bool
stree_siter_moveup(struct stree_iter *siter, uint64_t *keyp, int *depthp)
{
	diskblk_t localvalue;
	uint64_t key = *keyp;
	int depth = *depthp;
	uint64_t localkey;
	bool found;
	uint64_t oldkey;

	for (;;) {
		oldkey = key;

		/* Go as far up as possible to continue scanning right. */
		localkey = stree_localkey(key, depth);
		while (localkey + 1 == SRDXCAP) {

			if (siter->siter_nodes[depth] != NULL) {
				srdx_release(siter->siter_nodes[depth]);
				siter->siter_nodes[depth] = NULL;
			}

			if (depth == 0) {
				*depthp = depth;
				*keyp = key;
				return (false);
			}

			depth -= 1;
			localkey = stree_localkey(key, depth);
		}

		/* Move to the right. */
		stree_localkey_increment(&key, depth);

		found = stree_siter_moveright(
		    siter->siter_nodes[depth], &key, depth);
		if (found) {
			localkey = stree_localkey(key, depth);
			localvalue = SRDXVAL(
			    siter->siter_nodes[depth], localkey);
			assert(STREE_VALVALID(localvalue));
			break;
		}

		assert(oldkey != key);
	}

	*depthp = depth;
	*keyp = key;
	return (true);
}

/*
 * Find the closest extent to the right of the key. If the key
 * is in the middle of an extent, we ignore the part to the left.
 */
int
siter_start(int fd, uint64_t root, uint64_t key, struct stree_iter *siter)
{
	rdxnode srdx, schild;
	diskblk_t localvalue;
	uint64_t localkey;
	bool found;
	int error;
	int depth;

	siter->siter_key = key;
	for (depth = 0; depth < STREE_DEPTH; depth++)
		siter->siter_nodes[depth] = NULL;

	assert(key < KEYMAX);

	srdx_retrieve(fd, root, &srdx);

	for (depth = 0; depth < STREE_DEPTH; depth++) {
		siter->siter_nodes[depth] = srdx;

		srdx = siter->siter_nodes[depth];
		found = stree_siter_moveright(srdx, &key, depth);
		if (!found) {
			assert(depth > 0);

			found = stree_siter_moveup(siter, &key, &depth);
			assert(found);
			srdx = siter->siter_nodes[depth];
		}

		localkey = stree_localkey(key, depth);
		localvalue = SRDXVAL(srdx, localkey);
		assert(STREE_VALVALID(localvalue));

		if (depth == STREE_DEPTH - 1) {
			found = stree_siter_moveright(srdx, &key, depth);
			assert(found);
			break;
		}

		srdx_retrieve(fd, localvalue.offset, &schild);

		srdx = schild;
	}

	siter->siter_key = key;
	siter->siter_nodes[depth] = srdx;
	return (0);
}

/*
 * siter_keymin is our stree_iter start with modifications
 * stree_extent_next is the userspace siter_iter
 * stree_extent_find is similar to the existing one, but
 * uses the two functions above
 */

int
siter_iter(int fd, struct stree_iter *siter)
{
	rdxnode srdx = NULL, schild;
	uint64_t key = siter->siter_key;
	diskblk_t localvalue;
	uint64_t localkey;
	bool found;
	int depth;
	int error;

	if (key + 1 == KEYMAX)
		goto iterdone;

	depth = STREE_DEPTH - 1;
	found = stree_siter_moveup(siter, &key, &depth);
	if (!found)
		goto iterdone;

	srdx = siter->siter_nodes[depth];
	siter->siter_nodes[depth] = NULL;
	for (;;) {
		/* Move laterally. */
		found = stree_siter_moveright(srdx, &key, depth);
		assert(found);
		localkey = stree_localkey(key, depth);

		localvalue = SRDXVAL(srdx, localkey);
		assert(STREE_VALVALID(localvalue));

		if (depth == STREE_DEPTH - 1)
			break;

		/* Move downwards. */
		srdx_retrieve(fd, localvalue.offset, &schild);

		assert(siter->siter_nodes[depth] == NULL);
		siter->siter_nodes[depth] = srdx;
		srdx = schild;

		depth += 1;
	}

	siter->siter_key = key;
	siter->siter_nodes[depth] = srdx;

	return (0);

iterdone:
	siter_end(siter);
	if (srdx != NULL)
		srdx_release(srdx);

	return (EINVAL);
}

void
siter_end(struct stree_iter *siter)
{
	int depth;

	for (depth = 0; depth < STREE_DEPTH; depth++) {
		if (siter->siter_nodes[depth] == NULL)
			continue;
		srdx_release(siter->siter_nodes[depth]);
	}
}
