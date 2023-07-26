/*
 * ea_refcount.c
 *
 * Copyright (C) 2001 Theodore Ts'o.  This file may be
 * redistributed under the terms of the GNU Public License.
 */

#include "config.h"
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>
#include <stdio.h>

#ifdef TEST_PROGRAM
#undef ENABLE_NLS
#endif
#include "e2fsck.h"
#include "ext2fs/rbtree.h"

/*
 * The strategy we use for keeping track of EA refcounts is as
 * follows.  We keep the first EA blocks and its reference counts
 * in the rb-tree.  Once the refcount has dropped to zero, it is
 * removed from the rb-tree to save memory space.  Once the EA block is
 * checked, its bit is set in the block_ea_map bitmap.
 */
struct ea_refcount_el {
	struct rb_node	node;
	/* ea_key could either be an inode number or block number. */
	ea_key_t	ea_key;
	ea_value_t	ea_value;
};

struct ea_refcount {
	struct rb_root	root;
	struct rb_node	*cursor;
};

void ea_refcount_free(ext2_refcount_t refcount)
{
	struct ea_refcount_el *el;
	struct rb_node *node, *next;

	if (!refcount)
		return;

	for (node = ext2fs_rb_first(&refcount->root); node; node = next) {
		next = ext2fs_rb_next(node);
		el = ext2fs_rb_entry(node, struct ea_refcount_el, node);
		ext2fs_rb_erase(node, &refcount->root);
		ext2fs_free_mem(&el);
	}
	ext2fs_free_mem(&refcount);
}

errcode_t ea_refcount_create(ext2_refcount_t *ret)
{
	ext2_refcount_t	refcount;
	errcode_t	retval;

	retval = ext2fs_get_memzero(sizeof(struct ea_refcount), &refcount);
	if (retval)
		return retval;
	refcount->root = RB_ROOT;

	*ret = refcount;
	return 0;
}

/*
 * get_refcount_el() --- given an block number, try to find refcount
 * 	information in the rb-tree.  If the create flag is set,
 * 	and we can't find an entry, create and add it to rb-tree.
 */
static struct ea_refcount_el *get_refcount_el(ext2_refcount_t refcount,
					      ea_key_t ea_key, int create)
{
	struct rb_node		**node;
	struct rb_node		*parent = NULL;
	struct ea_refcount_el	*el;
	errcode_t		retval;

	if (!refcount)
		return 0;

	node = &refcount->root.rb_node;
	while (*node) {
		el = ext2fs_rb_entry(*node, struct ea_refcount_el, node);

		parent = *node;
		if (ea_key < el->ea_key)
			node = &(*node)->rb_left;
		else if (ea_key > el->ea_key)
			node = &(*node)->rb_right;
		else
			return el;
	}

	if (!create)
		return 0;

	retval = ext2fs_get_memzero(sizeof(struct ea_refcount_el), &el);
	if (retval)
		return 0;

	el->ea_key = ea_key;
	el->ea_value = 0;
	ext2fs_rb_link_node(&el->node, parent, node);
	ext2fs_rb_insert_color(&el->node, &refcount->root);

	return el;
}

errcode_t ea_refcount_fetch(ext2_refcount_t refcount, ea_key_t ea_key,
			    ea_value_t *ret)
{
	struct ea_refcount_el	*el;

	el = get_refcount_el(refcount, ea_key, 0);
	if (!el) {
		*ret = 0;
		return 0;
	}
	*ret = el->ea_value;
	return 0;
}

errcode_t ea_refcount_increment(ext2_refcount_t refcount, ea_key_t ea_key,
				ea_value_t *ret)
{
	struct ea_refcount_el	*el;

	el = get_refcount_el(refcount, ea_key, 1);
	if (!el)
		return EXT2_ET_NO_MEMORY;
	el->ea_value++;

	if (ret)
		*ret = el->ea_value;
	return 0;
}

errcode_t ea_refcount_decrement(ext2_refcount_t refcount, ea_key_t ea_key,
				ea_value_t *ret)
{
	struct ea_refcount_el	*el;

	el = get_refcount_el(refcount, ea_key, 0);
	if (!el)
		return EXT2_ET_INVALID_ARGUMENT;

	el->ea_value--;

	if (ret)
		*ret = el->ea_value;

	if (el->ea_value == 0) {
		ext2fs_rb_erase(&el->node, &refcount->root);
		ext2fs_free_mem(&el);
	}
	return 0;
}

errcode_t ea_refcount_store(ext2_refcount_t refcount, ea_key_t ea_key,
			    ea_value_t ea_value)
{
	struct ea_refcount_el	*el;

	/*
	 * Get the refcount element
	 */
	el = get_refcount_el(refcount, ea_key, ea_value ? 1 : 0);
	if (!el)
		return ea_value ? EXT2_ET_NO_MEMORY : 0;
	el->ea_value = ea_value;
	if (el->ea_value == 0) {
		ext2fs_rb_erase(&el->node, &refcount->root);
		ext2fs_free_mem(&el);
	}
	return 0;
}

void ea_refcount_intr_begin(ext2_refcount_t refcount)
{
	refcount->cursor = 0;
}

ea_key_t ea_refcount_intr_next(ext2_refcount_t refcount,
				ea_value_t *ret)
{
	struct ea_refcount_el	*el;
	struct rb_node		*node = refcount->cursor;

	if (node == NULL)
		node = ext2fs_rb_first(&refcount->root);
	else
		node = ext2fs_rb_next(node);

	if (node) {
		refcount->cursor = node;
		el = ext2fs_rb_entry(node, struct ea_refcount_el, node);
		if (ret)
			*ret = el->ea_value;
		return el->ea_key;
	}

	return 0;
}


#ifdef TEST_PROGRAM

errcode_t ea_refcount_validate(ext2_refcount_t refcount, FILE *out)
{
	struct ea_refcount_el	*el;
	struct rb_node		*node;
	ea_key_t		prev;
	int			prev_valid = 0;
	const char *bad = "bad refcount";

	for (node = ext2fs_rb_first(&refcount->root); node != NULL;
	     node = ext2fs_rb_next(node)) {
		el = ext2fs_rb_entry(node, struct ea_refcount_el, node);
		if (prev_valid && prev >= el->ea_key) {
			fprintf(out,
				"%s: prev.ea_key=%llu, curr.ea_key=%llu\n",
				bad,
				(unsigned long long) prev,
				(unsigned long long) el->ea_key);
			return EXT2_ET_INVALID_ARGUMENT;
		}
		prev = el->ea_key;
		prev_valid = 1;
	}

	return 0;
}

#define BCODE_END	0
#define BCODE_CREATE	1
#define BCODE_FREE	2
#define BCODE_STORE	3
#define BCODE_INCR	4
#define BCODE_DECR	5
#define BCODE_FETCH	6
#define BCODE_VALIDATE	7
#define BCODE_LIST	8

int bcode_program[] = {
	BCODE_CREATE,
	BCODE_STORE, 3, 3,
	BCODE_STORE, 4, 4,
	BCODE_STORE, 1, 1,
	BCODE_STORE, 8, 8,
	BCODE_STORE, 2, 2,
	BCODE_STORE, 4, 0,
	BCODE_STORE, 2, 0,
	BCODE_STORE, 6, 6,
	BCODE_VALIDATE,
	BCODE_STORE, 4, 4,
	BCODE_STORE, 2, 2,
	BCODE_FETCH, 1,
	BCODE_FETCH, 2,
	BCODE_INCR, 3,
	BCODE_INCR, 3,
	BCODE_DECR, 4,
	BCODE_STORE, 4, 4,
	BCODE_VALIDATE,
	BCODE_STORE, 20, 20,
	BCODE_STORE, 40, 40,
	BCODE_STORE, 30, 30,
	BCODE_STORE, 10, 10,
	BCODE_DECR, 30,
	BCODE_FETCH, 30,
	BCODE_DECR, 2,
	BCODE_DECR, 2,
	BCODE_LIST,
	BCODE_VALIDATE,
	BCODE_FREE,
	BCODE_END
};

int main(int argc, char **argv)
{
	int	i = 0;
	ext2_refcount_t refcount;
	ea_key_t	ea_key;
	ea_value_t	arg;
	errcode_t	retval;

	while (1) {
		switch (bcode_program[i++]) {
		case BCODE_END:
			exit(0);
		case BCODE_CREATE:
			retval = ea_refcount_create(&refcount);
			if (retval) {
				com_err("ea_refcount_create", retval,
					"while creating refcount");
				exit(1);
			} else
				printf("Creating refcount\n");
			break;
		case BCODE_FREE:
			ea_refcount_free(refcount);
			refcount = 0;
			printf("Freeing refcount\n");
			break;
		case BCODE_STORE:
			ea_key = (size_t) bcode_program[i++];
			arg = bcode_program[i++];
			printf("Storing ea_key %llu with value %llu\n",
			       (unsigned long long) ea_key,
			       (unsigned long long) arg);
			retval = ea_refcount_store(refcount, ea_key, arg);
			if (retval)
				com_err("ea_refcount_store", retval,
					"while storing ea_key %llu",
					(unsigned long long) ea_key);
			break;
		case BCODE_FETCH:
			ea_key = (size_t) bcode_program[i++];
			retval = ea_refcount_fetch(refcount, ea_key, &arg);
			if (retval)
				com_err("ea_refcount_fetch", retval,
					"while fetching ea_key %llu",
					(unsigned long long) ea_key);
			else
				printf("bcode_fetch(%llu) returns %llu\n",
				       (unsigned long long) ea_key,
				       (unsigned long long) arg);
			break;
		case BCODE_INCR:
			ea_key = (size_t) bcode_program[i++];
			retval = ea_refcount_increment(refcount, ea_key, &arg);
			if (retval)
				com_err("ea_refcount_increment", retval,
					"while incrementing ea_key %llu",
					(unsigned long long) ea_key);
			else
				printf("bcode_increment(%llu) returns %llu\n",
				       (unsigned long long) ea_key,
				       (unsigned long long) arg);
			break;
		case BCODE_DECR:
			ea_key = (size_t) bcode_program[i++];
			retval = ea_refcount_decrement(refcount, ea_key, &arg);
			if (retval)
				com_err("ea_refcount_decrement", retval,
					"while decrementing ea_key %llu",
					(unsigned long long) ea_key);
			else
				printf("bcode_decrement(%llu) returns %llu\n",
				       (unsigned long long) ea_key,
				       (unsigned long long) arg);
			break;
		case BCODE_VALIDATE:
			retval = ea_refcount_validate(refcount, stderr);
			if (retval)
				com_err("ea_refcount_validate", retval,
					"while validating");
			else
				printf("Refcount validation OK.\n");
			break;
		case BCODE_LIST:
			ea_refcount_intr_begin(refcount);
			while (1) {
				ea_key = ea_refcount_intr_next(refcount, &arg);
				if (!ea_key)
					break;
				printf("\tea_key=%llu, count=%llu\n",
				       (unsigned long long) ea_key,
				       (unsigned long long) arg);
			}
			break;
		}

	}
}

#endif
