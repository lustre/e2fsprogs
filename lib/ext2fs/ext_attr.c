/*
 * ext_attr.c --- extended attribute blocks
 *
 * Copyright (C) 2001 Andreas Gruenbacher, <a.gruenbacher@computer.org>
 *
 * Copyright (C) 2002 Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Library
 * General Public License, version 2.
 * %End-Header%
 */

#include "config.h"
#include <stdio.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>
#include <time.h>
#include <errno.h>
#if defined HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#elif defined HAVE_ATTR_XATTR_H
#include <attr/xattr.h>
#endif

#include "ext2_fs.h"
#include "ext2_ext_attr.h"
#include "ext4_acl.h"

#include "ext2fs.h"

static errcode_t read_ea_inode_hash(ext2_filsys fs, ext2_ino_t ino, __u32 *hash)
{
	struct ext2_inode inode;
	errcode_t retval;

	retval = ext2fs_read_inode(fs, ino, &inode);
	if (retval)
		return retval;
	*hash = ext2fs_get_ea_inode_hash(&inode);
	return 0;
}

#define NAME_HASH_SHIFT 5
#define VALUE_HASH_SHIFT 16

/*
 * ext2_xattr_hash_entry()
 *
 * Compute the hash of an extended attribute.
 */
__u32 ext2fs_ext_attr_hash_entry(struct ext2_ext_attr_entry *entry, void *data)
{
	__u32 hash = 0;
	char *name = ((char *) entry) + sizeof(struct ext2_ext_attr_entry);
	int n;

	for (n = 0; n < entry->e_name_len; n++) {
		hash = (hash << NAME_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - NAME_HASH_SHIFT)) ^
		       *name++;
	}

	/* The hash needs to be calculated on the data in little-endian. */
	if (entry->e_value_inum == 0 && entry->e_value_size != 0) {
		__u32 *value = (__u32 *)data;
		for (n = (entry->e_value_size + EXT2_EXT_ATTR_ROUND) >>
			 EXT2_EXT_ATTR_PAD_BITS; n; n--) {
			hash = (hash << VALUE_HASH_SHIFT) ^
			       (hash >> (8*sizeof(hash) - VALUE_HASH_SHIFT)) ^
			       ext2fs_le32_to_cpu(*value++);
		}
	}

	return hash;
}

/*
 * ext2fs_ext_attr_hash_entry2()
 *
 * Compute the hash of an extended attribute.
 * This version of the function supports hashing entries that reference
 * external inodes (ea_inode feature).
 */
errcode_t ext2fs_ext_attr_hash_entry2(ext2_filsys fs,
				      struct ext2_ext_attr_entry *entry,
				      void *data, __u32 *hash)
{
	*hash = ext2fs_ext_attr_hash_entry(entry, data);

	if (entry->e_value_inum) {
		__u32 ea_inode_hash;
		errcode_t retval;

		retval = read_ea_inode_hash(fs, entry->e_value_inum,
					    &ea_inode_hash);
		if (retval)
			return retval;

		*hash = (*hash << VALUE_HASH_SHIFT) ^
			(*hash >> (8*sizeof(*hash) - VALUE_HASH_SHIFT)) ^
			ea_inode_hash;
	}
	return 0;
}

#undef NAME_HASH_SHIFT
#undef VALUE_HASH_SHIFT

#define BLOCK_HASH_SHIFT 16

/* Mirrors ext4_xattr_rehash() implementation in kernel. */
void ext2fs_ext_attr_block_rehash(struct ext2_ext_attr_header *header,
				  struct ext2_ext_attr_entry *end)
{
	struct ext2_ext_attr_entry *here;
	__u32 hash = 0;

	here = (struct ext2_ext_attr_entry *)(header+1);
	while (here < end && !EXT2_EXT_IS_LAST_ENTRY(here)) {
		if (!here->e_hash) {
			/* Block is not shared if an entry's hash value == 0 */
			hash = 0;
			break;
		}
		hash = (hash << BLOCK_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - BLOCK_HASH_SHIFT)) ^
		       here->e_hash;
		here = EXT2_EXT_ATTR_NEXT(here);
	}
	header->h_hash = hash;
}

/*
 * Re-compute the extended attribute hash value after an entry has changed.
 */
static void ext2fs_attr_rehash(struct ext2_ext_attr_header *header,
				struct ext2_ext_attr_entry *entry)
{
	struct ext2_ext_attr_entry *here;
	__u32 hash = 0;

	entry->e_hash = ext2fs_ext_attr_hash_entry(entry, (char *)header +
						   entry->e_value_offs);

	here = ENTRY(header+1);
	while (!EXT2_EXT_IS_LAST_ENTRY(here)) {
		if (!here->e_hash) {
			/* Block is not shared if an entry's hash value == 0 */
			hash = 0;
			break;
		}
		hash = (hash << BLOCK_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - BLOCK_HASH_SHIFT)) ^
		       here->e_hash;
		here = EXT2_EXT_ATTR_NEXT(here);
	}
	header->h_hash = hash;
}

#undef BLOCK_HASH_SHIFT

__u32 ext2fs_get_ea_inode_hash(struct ext2_inode *inode)
{
	return inode->i_atime;
}

void ext2fs_set_ea_inode_hash(struct ext2_inode *inode, __u32 hash)
{
	inode->i_atime = hash;
}

__u64 ext2fs_get_ea_inode_ref(struct ext2_inode *inode)
{
	return ((__u64)inode->i_ctime << 32) | inode->osd1.linux1.l_i_version;
}

void ext2fs_set_ea_inode_ref(struct ext2_inode *inode, __u64 ref_count)
{
	inode->i_ctime = (__u32)(ref_count >> 32);
	inode->osd1.linux1.l_i_version = (__u32)ref_count;
}

static errcode_t check_ext_attr_header(struct ext2_ext_attr_header *header)
{
	if ((header->h_magic != EXT2_EXT_ATTR_MAGIC_v1 &&
	     header->h_magic != EXT2_EXT_ATTR_MAGIC) ||
	    header->h_blocks != 1)
		return EXT2_ET_BAD_EA_HEADER;

	return 0;
}

errcode_t ext2fs_read_ext_attr3(ext2_filsys fs, blk64_t block, void *buf,
				ext2_ino_t inum)
{
	int		csum_failed = 0;
	errcode_t	retval;

	retval = io_channel_read_blk64(fs->io, block, 1, buf);
	if (retval)
		return retval;

	if (!(fs->flags & EXT2_FLAG_IGNORE_CSUM_ERRORS) &&
	    !ext2fs_ext_attr_block_csum_verify(fs, inum, block, buf))
		csum_failed = 1;

#ifdef WORDS_BIGENDIAN
	ext2fs_swap_ext_attr(buf, buf, fs->blocksize, 1);
#endif

	retval = check_ext_attr_header(buf);
	if (retval == 0 && csum_failed)
		retval = EXT2_ET_EXT_ATTR_CSUM_INVALID;

	return retval;
}

errcode_t ext2fs_read_ext_attr2(ext2_filsys fs, blk64_t block, void *buf)
{
	return ext2fs_read_ext_attr3(fs, block, buf, 0);
}

errcode_t ext2fs_read_ext_attr(ext2_filsys fs, blk_t block, void *buf)
{
	return ext2fs_read_ext_attr2(fs, block, buf);
}

errcode_t ext2fs_write_ext_attr3(ext2_filsys fs, blk64_t block, void *inbuf,
				 ext2_ino_t inum)
{
	errcode_t	retval;
	char		*write_buf;

#ifdef WORDS_BIGENDIAN
	retval = ext2fs_get_mem(fs->blocksize, &write_buf);
	if (retval)
		return retval;
	ext2fs_swap_ext_attr(write_buf, inbuf, fs->blocksize, 1);
#else
	write_buf = (char *) inbuf;
#endif

	retval = ext2fs_ext_attr_block_csum_set(fs, inum, block,
			(struct ext2_ext_attr_header *)write_buf);
	if (retval)
		return retval;

	retval = io_channel_write_blk64(fs->io, block, 1, write_buf);
#ifdef WORDS_BIGENDIAN
	ext2fs_free_mem(&write_buf);
#endif
	if (!retval)
		ext2fs_mark_changed(fs);
	return retval;
}

errcode_t ext2fs_write_ext_attr2(ext2_filsys fs, blk64_t block, void *inbuf)
{
	return ext2fs_write_ext_attr3(fs, block, inbuf, 0);
}

errcode_t ext2fs_write_ext_attr(ext2_filsys fs, blk_t block, void *inbuf)
{
	return ext2fs_write_ext_attr2(fs, block, inbuf);
}

/*
 * This function adjusts the reference count of the EA block.
 */
errcode_t ext2fs_adjust_ea_refcount3(ext2_filsys fs, blk64_t blk,
				    char *block_buf, int adjust,
				    __u32 *newcount, ext2_ino_t inum)
{
	errcode_t	retval;
	struct ext2_ext_attr_header *header;
	char	*buf = 0;

	if ((blk >= ext2fs_blocks_count(fs->super)) ||
	    (blk < fs->super->s_first_data_block))
		return EXT2_ET_BAD_EA_BLOCK_NUM;

	if (!block_buf) {
		retval = ext2fs_get_mem(fs->blocksize, &buf);
		if (retval)
			return retval;
		block_buf = buf;
	}

	retval = ext2fs_read_ext_attr3(fs, blk, block_buf, inum);
	if (retval)
		goto errout;

	header = BHDR(block_buf);
	if (header->h_magic != EXT2_EXT_ATTR_MAGIC)
		return EXT2_ET_EA_BAD_MAGIC;

	header->h_refcount += adjust;
	if (newcount)
		*newcount = header->h_refcount;

	retval = ext2fs_write_ext_attr3(fs, blk, block_buf, inum);
	if (retval)
		goto errout;

errout:
	if (buf)
		ext2fs_free_mem(&buf);
	return retval;
}

errcode_t ext2fs_adjust_ea_refcount2(ext2_filsys fs, blk64_t blk,
				    char *block_buf, int adjust,
				    __u32 *newcount)
{
	return ext2fs_adjust_ea_refcount3(fs, blk, block_buf, adjust,
					  newcount, 0);
}

errcode_t ext2fs_adjust_ea_refcount(ext2_filsys fs, blk_t blk,
					char *block_buf, int adjust,
					__u32 *newcount)
{
	return ext2fs_adjust_ea_refcount2(fs, blk, block_buf, adjust,
					  newcount);
}

/* Manipulate the contents of extended attribute regions */
struct ext2_xattr {
	int name_index;
	char *name;
	char *short_name;
	void *value;
	unsigned int value_len;
	ext2_ino_t ea_ino;
};

struct ext2_xattr_handle {
	errcode_t magic;
	ext2_filsys fs;
	struct ext2_xattr *attrs;
	int capacity;
	int count;
	int ibody_count;
	ext2_ino_t ino;
	unsigned int flags;
};

static errcode_t ext2fs_xattrs_expand(struct ext2_xattr_handle *h,
				      unsigned int expandby)
{
	struct ext2_xattr *new_attrs;
	errcode_t err;

	err = ext2fs_get_arrayzero(h->capacity + expandby,
				   sizeof(struct ext2_xattr), &new_attrs);
	if (err)
		return err;

	memcpy(new_attrs, h->attrs, h->capacity * sizeof(struct ext2_xattr));
	ext2fs_free_mem(&h->attrs);
	h->capacity += expandby;
	h->attrs = new_attrs;

	return 0;
}

struct ea_name_index {
	int index;
	const char *name;
};

/* Keep these names sorted in order of decreasing specificity. */
static struct ea_name_index ea_names[] = {
	{10, "gnu."},
	{3, "system.posix_acl_default"},
	{2, "system.posix_acl_access"},
	{8, "system.richacl"},
	{6, "security."},
	{5, "lustre."},
	{4, "trusted."},
	{7, "system."},
	{1, "user."},
	{0, NULL},
};

static const char *find_ea_prefix(int index)
{
	struct ea_name_index *e;

	for (e = ea_names; e->name; e++)
		if (e->index == index)
			return e->name;

	return NULL;
}

static int find_ea_index(const char *fullname, const char **name, int *index)
{
	struct ea_name_index *e;

	for (e = ea_names; e->name; e++) {
		if (strncmp(fullname, e->name, strlen(e->name)) == 0) {
			*name = fullname + strlen(e->name);
			*index = e->index;
			return 1;
		}
	}
	return 0;
}

errcode_t ext2fs_free_ext_attr(ext2_filsys fs, ext2_ino_t ino,
			       struct ext2_inode_large *inode)
{
	struct ext2_ext_attr_header *header;
	void *block_buf = NULL;
	blk64_t blk;
	errcode_t err;
	struct ext2_inode_large i;

	/* Read inode? */
	if (inode == NULL) {
		err = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)&i,
					     sizeof(struct ext2_inode_large));
		if (err)
			return err;
		inode = &i;
	}

	/* Do we already have an EA block? */
	blk = ext2fs_file_acl_block(fs, (struct ext2_inode *)inode);
	if (blk == 0)
		return 0;

	/* Find block, zero it, write back */
	if ((blk < fs->super->s_first_data_block) ||
	    (blk >= ext2fs_blocks_count(fs->super))) {
		err = EXT2_ET_BAD_EA_BLOCK_NUM;
		goto out;
	}

	err = ext2fs_get_mem(fs->blocksize, &block_buf);
	if (err)
		goto out;

	err = ext2fs_read_ext_attr3(fs, blk, block_buf, ino);
	if (err)
		goto out2;

	/* We only know how to deal with v2 EA blocks */
	header = (struct ext2_ext_attr_header *) block_buf;
	if (header->h_magic != EXT2_EXT_ATTR_MAGIC) {
		err = EXT2_ET_BAD_EA_HEADER;
		goto out2;
	}

	header->h_refcount--;
	err = ext2fs_write_ext_attr3(fs, blk, block_buf, ino);
	if (err)
		goto out2;

	/* Erase link to block */
	ext2fs_file_acl_block_set(fs, (struct ext2_inode *)inode, 0);
	if (header->h_refcount == 0)
		ext2fs_block_alloc_stats2(fs, blk, -1);
	err = ext2fs_iblk_sub_blocks(fs, (struct ext2_inode *)inode, 1);
	if (err)
		goto out2;

	/* Write inode? */
	if (inode == &i) {
		err = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)&i,
					      sizeof(struct ext2_inode_large));
		if (err)
			goto out2;
	}

out2:
	ext2fs_free_mem(&block_buf);
out:
	return err;
}

static errcode_t prep_ea_block_for_write(ext2_filsys fs, ext2_ino_t ino,
					 struct ext2_inode_large *inode)
{
	struct ext2_ext_attr_header *header;
	void *block_buf = NULL;
	blk64_t blk, goal;
	errcode_t err;

	/* Do we already have an EA block? */
	blk = ext2fs_file_acl_block(fs, (struct ext2_inode *)inode);
	if (blk != 0) {
		if ((blk < fs->super->s_first_data_block) ||
		    (blk >= ext2fs_blocks_count(fs->super))) {
			err = EXT2_ET_BAD_EA_BLOCK_NUM;
			goto out;
		}

		err = ext2fs_get_mem(fs->blocksize, &block_buf);
		if (err)
			goto out;

		err = ext2fs_read_ext_attr3(fs, blk, block_buf, ino);
		if (err)
			goto out2;

		/* We only know how to deal with v2 EA blocks */
		header = (struct ext2_ext_attr_header *) block_buf;
		if (header->h_magic != EXT2_EXT_ATTR_MAGIC) {
			err = EXT2_ET_BAD_EA_HEADER;
			goto out2;
		}

		/* Single-user block.  We're done here. */
		if (header->h_refcount == 1)
			goto out2;

		/* We need to CoW the block. */
		header->h_refcount--;
		err = ext2fs_write_ext_attr3(fs, blk, block_buf, ino);
		if (err)
			goto out2;
	} else {
		/* No block, we must increment i_blocks */
		err = ext2fs_iblk_add_blocks(fs, (struct ext2_inode *)inode,
					     1);
		if (err)
			goto out;
	}

	/* Allocate a block */
	goal = ext2fs_find_inode_goal(fs, ino, (struct ext2_inode *)inode, 0);
	err = ext2fs_alloc_block2(fs, goal, NULL, &blk);
	if (err)
		goto out2;
	ext2fs_file_acl_block_set(fs, (struct ext2_inode *)inode, blk);
out2:
	if (block_buf)
		ext2fs_free_mem(&block_buf);
out:
	return err;
}


static inline int
posix_acl_xattr_count(size_t size)
{
        if (size < sizeof(posix_acl_xattr_header))
                return -1;
        size -= sizeof(posix_acl_xattr_header);
        if (size % sizeof(posix_acl_xattr_entry))
                return -1;
        return size / sizeof(posix_acl_xattr_entry);
}

/*
 * The lgetxattr function returns data formatted in the POSIX extended
 * attribute format.  The on-disk format uses a more compact encoding.
 * See the ext4_acl_to_disk in fs/ext4/acl.c.
 */
static errcode_t convert_posix_acl_to_disk_buffer(const void *value, size_t size,
						  void *out_buf, size_t *size_out)
{
	const posix_acl_xattr_header *header =
		(const posix_acl_xattr_header*) value;
	const posix_acl_xattr_entry *end, *entry =
		(const posix_acl_xattr_entry *)(header+1);
	ext4_acl_header *ext_acl;
	size_t s;
	char *e;

	int count;

	if (!value)
		return EINVAL;
	if (size < sizeof(posix_acl_xattr_header))
		return ENOMEM;
	if (header->a_version != ext2fs_cpu_to_le32(POSIX_ACL_XATTR_VERSION))
		return EINVAL;

	count = posix_acl_xattr_count(size);
	ext_acl = out_buf;
	ext_acl->a_version = ext2fs_cpu_to_le32(EXT4_ACL_VERSION);

	if (count <= 0)
		return EINVAL;

	e = (char *) out_buf + sizeof(ext4_acl_header);
	s = sizeof(ext4_acl_header);
	for (end = entry + count; entry != end;entry++) {
		ext4_acl_entry *disk_entry = (ext4_acl_entry*) e;
		disk_entry->e_tag = entry->e_tag;
		disk_entry->e_perm = entry->e_perm;

		switch(ext2fs_le16_to_cpu(entry->e_tag)) {
			case ACL_USER_OBJ:
			case ACL_GROUP_OBJ:
			case ACL_MASK:
			case ACL_OTHER:
				e += sizeof(ext4_acl_entry_short);
				s += sizeof(ext4_acl_entry_short);
				break;
			case ACL_USER:
			case ACL_GROUP:
				disk_entry->e_id = entry->e_id;
				e += sizeof(ext4_acl_entry);
				s += sizeof(ext4_acl_entry);
				break;
			default:
				return EINVAL;
		}
	}
	*size_out = s;
	return 0;
}

static errcode_t convert_disk_buffer_to_posix_acl(const void *value, size_t size,
						  void **out_buf, size_t *size_out)
{
	posix_acl_xattr_header *header;
	posix_acl_xattr_entry *entry;
	const ext4_acl_header *ext_acl = (const ext4_acl_header *) value;
	errcode_t err;
	const char *cp;
	char *out;

	if ((!value) ||
	    (size < sizeof(ext4_acl_header)) ||
	    (ext_acl->a_version != ext2fs_cpu_to_le32(EXT4_ACL_VERSION)))
		return EINVAL;

	err = ext2fs_get_mem(size * 2, &out);
	if (err)
		return err;

	header = (posix_acl_xattr_header *) out;
	header->a_version = ext2fs_cpu_to_le32(POSIX_ACL_XATTR_VERSION);
	entry = (posix_acl_xattr_entry *) (out + sizeof(posix_acl_xattr_header));

	cp = (const char *) value + sizeof(ext4_acl_header);
	size -= sizeof(ext4_acl_header);

	while (size > 0) {
		const ext4_acl_entry *disk_entry = (const ext4_acl_entry *) cp;

		entry->e_tag = disk_entry->e_tag;
		entry->e_perm = disk_entry->e_perm;

		switch(ext2fs_le16_to_cpu(entry->e_tag)) {
			case ACL_USER_OBJ:
			case ACL_GROUP_OBJ:
			case ACL_MASK:
			case ACL_OTHER:
				entry->e_id = 0;
				cp += sizeof(ext4_acl_entry_short);
				size -= sizeof(ext4_acl_entry_short);
				break;
			case ACL_USER:
			case ACL_GROUP:
				entry->e_id = disk_entry->e_id;
				cp += sizeof(ext4_acl_entry);
				size -= sizeof(ext4_acl_entry);
				break;
			default:
				ext2fs_free_mem(&out);
				return EINVAL;
		}
		entry++;
	}
	*out_buf = out;
	*size_out = ((char *) entry - out);
	return 0;
}

static errcode_t
write_xattrs_to_buffer(ext2_filsys fs, struct ext2_xattr *attrs, int count,
		       void *entries_start, unsigned int storage_size,
		       unsigned int value_offset_correction, int write_hash)
{
	struct ext2_xattr *x;
	struct ext2_ext_attr_entry *e = entries_start;
	char *end = (char *) entries_start + storage_size;
	unsigned int value_size;
	errcode_t err;

	memset(entries_start, 0, storage_size);
	for (x = attrs; x < attrs + count; x++) {
		value_size = ((x->value_len + EXT2_EXT_ATTR_PAD - 1) /
			      EXT2_EXT_ATTR_PAD) * EXT2_EXT_ATTR_PAD;

		/* Fill out e appropriately */
		e->e_name_len = strlen(x->short_name);
		e->e_name_index = x->name_index;

		e->e_value_size = x->value_len;
		e->e_value_inum = x->ea_ino;

		/* Store name */
		memcpy((char *)e + sizeof(*e), x->short_name, e->e_name_len);
		if (x->ea_ino) {
			e->e_value_offs = 0;
		} else {
			end -= value_size;
			e->e_value_offs = end - (char *) entries_start +
						value_offset_correction;
			memcpy(end, x->value, e->e_value_size);
		}

		if (write_hash || x->ea_ino) {
			err = ext2fs_ext_attr_hash_entry2(fs, e,
							  x->ea_ino ? 0 : end,
							  &e->e_hash);
			if (err)
				return err;
		} else
			e->e_hash = 0;

		e = EXT2_EXT_ATTR_NEXT(e);
		*(__u32 *)e = 0;
	}
	return 0;
}

errcode_t ext2fs_xattrs_write(struct ext2_xattr_handle *handle)
{
	ext2_filsys fs = handle->fs;
	const unsigned int inode_size = EXT2_INODE_SIZE(fs->super);
	struct ext2_inode_large *inode;
	char *start, *block_buf = NULL;
	struct ext2_ext_attr_header *header;
	__u32 ea_inode_magic;
	blk64_t blk;
	unsigned int storage_size;
	unsigned int i;
	errcode_t err;

	EXT2_CHECK_MAGIC(handle, EXT2_ET_MAGIC_EA_HANDLE);
	i = inode_size;
	if (i < sizeof(*inode))
		i = sizeof(*inode);
	err = ext2fs_get_memzero(i, &inode);
	if (err)
		return err;

	err = ext2fs_read_inode_full(fs, handle->ino, EXT2_INODE(inode),
				     inode_size);
	if (err)
		goto out;

	/* If extra_isize isn't set, we need to set it now */
	if (inode->i_extra_isize == 0 &&
	    inode_size > EXT2_GOOD_OLD_INODE_SIZE) {
		char *p = (char *)inode;
		size_t extra = fs->super->s_want_extra_isize;

		if (extra == 0)
			extra = sizeof(__u32);
		memset(p + EXT2_GOOD_OLD_INODE_SIZE, 0, extra);
		inode->i_extra_isize = extra;
	}
	if (inode->i_extra_isize & 3) {
		err = EXT2_ET_INODE_CORRUPTED;
		goto out;
	}

	/* Does the inode have space for EA? */
	if (inode->i_extra_isize < sizeof(inode->i_extra_isize) ||
	    inode_size <= EXT2_GOOD_OLD_INODE_SIZE + inode->i_extra_isize +
								sizeof(__u32))
		goto write_ea_block;

	/* Write the inode EA */
	ea_inode_magic = EXT2_EXT_ATTR_MAGIC;
	memcpy(((char *) inode) + EXT2_GOOD_OLD_INODE_SIZE +
	       inode->i_extra_isize, &ea_inode_magic, sizeof(__u32));
	storage_size = inode_size - EXT2_GOOD_OLD_INODE_SIZE -
				inode->i_extra_isize - sizeof(__u32);
	start = ((char *) inode) + EXT2_GOOD_OLD_INODE_SIZE +
				inode->i_extra_isize + sizeof(__u32);

	err = write_xattrs_to_buffer(fs, handle->attrs, handle->ibody_count,
				     start, storage_size, 0, 0);
	if (err)
		goto out;
write_ea_block:
	/* Are we done? */
	if (handle->ibody_count == handle->count &&
	    !ext2fs_file_acl_block(fs, EXT2_INODE(inode)))
		goto skip_ea_block;

	/* Write the EA block */
	err = ext2fs_get_memzero(fs->blocksize, &block_buf);
	if (err)
		goto out;

	storage_size = fs->blocksize - sizeof(struct ext2_ext_attr_header);
	start = block_buf + sizeof(struct ext2_ext_attr_header);

	err = write_xattrs_to_buffer(fs, handle->attrs + handle->ibody_count,
				     handle->count - handle->ibody_count, start,
				     storage_size, start - block_buf, 1);
	if (err)
		goto out2;

	/* Write a header on the EA block */
	header = (struct ext2_ext_attr_header *) block_buf;
	header->h_magic = EXT2_EXT_ATTR_MAGIC;
	header->h_refcount = 1;
	header->h_blocks = 1;

	/* Get a new block for writing */
	err = prep_ea_block_for_write(fs, handle->ino, inode);
	if (err)
		goto out2;

	/* Finally, write the new EA block */
	blk = ext2fs_file_acl_block(fs, EXT2_INODE(inode));
	err = ext2fs_write_ext_attr3(fs, blk, block_buf, handle->ino);
	if (err)
		goto out2;

skip_ea_block:
	blk = ext2fs_file_acl_block(fs, (struct ext2_inode *)inode);
	if (!block_buf && blk) {
		/* xattrs shrunk, free the block */
		err = ext2fs_free_ext_attr(fs, handle->ino, inode);
		if (err)
			goto out;
	}

	/* Write the inode */
	err = ext2fs_write_inode_full(fs, handle->ino, EXT2_INODE(inode),
				      inode_size);
	if (err)
		goto out2;

out2:
	ext2fs_free_mem(&block_buf);
out:
	ext2fs_free_mem(&inode);
	return err;
}

static errcode_t read_xattrs_from_buffer(struct ext2_xattr_handle *handle,
					 struct ext2_inode_large *inode,
					 struct ext2_ext_attr_entry *entries,
					 unsigned int storage_size,
					 char *value_start)
{
	struct ext2_xattr *x;
	struct ext2_ext_attr_entry *entry, *end;
	const char *prefix;
	unsigned int remain, prefix_len;
	errcode_t err;
	unsigned int values_size = storage_size +
			((char *)entries - value_start);

	/* find the end */
	end = entries;
	remain = storage_size;
	while (remain >= sizeof(struct ext2_ext_attr_entry) &&
	       !EXT2_EXT_IS_LAST_ENTRY(end)) {

		/* header eats this space */
		remain -= sizeof(struct ext2_ext_attr_entry);

		/* is attribute name valid? */
		if (EXT2_EXT_ATTR_SIZE(end->e_name_len) > remain)
			return EXT2_ET_EA_BAD_NAME_LEN;

		/* attribute len eats this space */
		remain -= EXT2_EXT_ATTR_SIZE(end->e_name_len);
		end = EXT2_EXT_ATTR_NEXT(end);
	}

	entry = entries;
	remain = storage_size;
	while (remain >= sizeof(struct ext2_ext_attr_entry) &&
	       !EXT2_EXT_IS_LAST_ENTRY(entry)) {

		/* Allocate space for more attrs? */
		if (handle->count == handle->capacity) {
			err = ext2fs_xattrs_expand(handle, 4);
			if (err)
				return err;
		}

		x = handle->attrs + handle->count;

		/* header eats this space */
		remain -= sizeof(struct ext2_ext_attr_entry);

		/* attribute len eats this space */
		remain -= EXT2_EXT_ATTR_SIZE(entry->e_name_len);

		/* Extract name */
		prefix = find_ea_prefix(entry->e_name_index);
		prefix_len = (prefix ? strlen(prefix) : 0);
		err = ext2fs_get_memzero(entry->e_name_len + prefix_len + 1,
					 &x->name);
		if (err)
			return err;
		if (prefix)
			memcpy(x->name, prefix, prefix_len);
		if (entry->e_name_len)
			memcpy(x->name + prefix_len,
			       (char *)entry + sizeof(*entry),
			       entry->e_name_len);
		x->short_name = x->name + prefix_len;
		x->name_index = entry->e_name_index;

		/* Check & copy value */
		if (!ext2fs_has_feature_ea_inode(handle->fs->super) &&
		    entry->e_value_inum != 0)
			return EXT2_ET_BAD_EA_BLOCK_NUM;

		if (entry->e_value_inum == 0) {
			if (entry->e_value_size > remain)
				return EXT2_ET_EA_BAD_VALUE_SIZE;

			if (entry->e_value_offs + entry->e_value_size > values_size)
				return EXT2_ET_EA_BAD_VALUE_OFFSET;

			if (entry->e_value_size > 0 &&
			    value_start + entry->e_value_offs <
			    (char *)end + sizeof(__u32))
				return EXT2_ET_EA_BAD_VALUE_OFFSET;

			remain -= entry->e_value_size;

			err = ext2fs_get_mem(entry->e_value_size, &x->value);
			if (err)
				return err;
			memcpy(x->value, value_start + entry->e_value_offs,
			       entry->e_value_size);
		} else {
			struct ext2_inode *ea_inode;
			ext2_file_t ea_file;

			if (entry->e_value_offs != 0)
				return EXT2_ET_EA_BAD_VALUE_OFFSET;

			if (entry->e_value_size > (64 * 1024))
				return EXT2_ET_EA_BAD_VALUE_SIZE;

			err = ext2fs_get_mem(entry->e_value_size, &x->value);
			if (err)
				return err;

			err = ext2fs_file_open(handle->fs, entry->e_value_inum,
					       0, &ea_file);
			if (err)
				return err;

			ea_inode = ext2fs_file_get_inode(ea_file);
			if ((ea_inode->i_flags & EXT4_INLINE_DATA_FL) ||
			    !(ea_inode->i_flags & EXT4_EA_INODE_FL) ||
			    ea_inode->i_links_count == 0)
				err = EXT2_ET_EA_INODE_CORRUPTED;
			else if ((__u64) ext2fs_file_get_size(ea_file) !=
				 entry->e_value_size)
				err = EXT2_ET_EA_BAD_VALUE_SIZE;
			else
				err = ext2fs_file_read(ea_file, x->value,
						       entry->e_value_size, 0);
			ext2fs_file_close(ea_file);
			if (err)
				return err;
		}

		x->ea_ino = entry->e_value_inum;
		x->value_len = entry->e_value_size;

		/* e_hash may be 0 in older inode's ea */
		if (entry->e_hash != 0) {
			__u32 hash;
			void *data = (entry->e_value_inum != 0) ?
					0 : value_start + entry->e_value_offs;

			err = ext2fs_ext_attr_hash_entry2(handle->fs, entry,
							  data, &hash);
			if (err)
				return err;
			if (entry->e_hash != hash) {
				struct ext2_inode child;

				/* Check whether this is an old Lustre-style
				 * ea_inode reference.
				 */
				err = ext2fs_read_inode(handle->fs,
							entry->e_value_inum,
							&child);
				if (err)
					return err;
				if (child.i_mtime != handle->ino ||
				    child.i_generation != inode->i_generation)
					return EXT2_ET_BAD_EA_HASH;
			}
		}

		handle->count++;
		entry = EXT2_EXT_ATTR_NEXT(entry);
	}

	return 0;
}

static void xattrs_free_keys(struct ext2_xattr_handle *h)
{
	struct ext2_xattr *a = h->attrs;
	int i;

	for (i = 0; i < h->capacity; i++) {
		if (a[i].name)
			ext2fs_free_mem(&a[i].name);
		if (a[i].value)
			ext2fs_free_mem(&a[i].value);
	}
	h->count = 0;
	h->ibody_count = 0;
}

/* fetch xattrs from an already-loaded inode */
errcode_t ext2fs_xattrs_read_inode(struct ext2_xattr_handle *handle,
				   struct ext2_inode_large *inode)
{

	struct ext2_ext_attr_header *header;
	__u32 ea_inode_magic;
	unsigned int storage_size;
	char *start, *block_buf = NULL;
	blk64_t blk;
	errcode_t err;

	EXT2_CHECK_MAGIC(handle, EXT2_ET_MAGIC_EA_HANDLE);

	xattrs_free_keys(handle);

	/* Does the inode have space for EA? */
	if (inode->i_extra_isize < sizeof(inode->i_extra_isize) ||
	    EXT2_INODE_SIZE(handle->fs->super) <= EXT2_GOOD_OLD_INODE_SIZE +
						  inode->i_extra_isize +
						  sizeof(__u32))
		goto read_ea_block;
	if (inode->i_extra_isize & 3) {
		err = EXT2_ET_INODE_CORRUPTED;
		goto out;
	}

	/* Look for EA in the inode */
	memcpy(&ea_inode_magic, ((char *) inode) + EXT2_GOOD_OLD_INODE_SIZE +
	       inode->i_extra_isize, sizeof(__u32));
	if (ea_inode_magic == EXT2_EXT_ATTR_MAGIC) {
		storage_size = EXT2_INODE_SIZE(handle->fs->super) -
			EXT2_GOOD_OLD_INODE_SIZE - inode->i_extra_isize -
			sizeof(__u32);
		start = ((char *) inode) + EXT2_GOOD_OLD_INODE_SIZE +
			inode->i_extra_isize + sizeof(__u32);

		err = read_xattrs_from_buffer(handle, inode,
					(struct ext2_ext_attr_entry *) start,
					storage_size, start);
		if (err)
			goto out;

		handle->ibody_count = handle->count;
	}

read_ea_block:
	/* Look for EA in a separate EA block */
	blk = ext2fs_file_acl_block(handle->fs, EXT2_INODE(inode));
	if (blk != 0) {
		if ((blk < handle->fs->super->s_first_data_block) ||
		    (blk >= ext2fs_blocks_count(handle->fs->super))) {
			err = EXT2_ET_BAD_EA_BLOCK_NUM;
			goto out;
		}

		err = ext2fs_get_mem(handle->fs->blocksize, &block_buf);
		if (err)
			goto out;

		err = ext2fs_read_ext_attr3(handle->fs, blk, block_buf,
					    handle->ino);
		if (err)
			goto out3;

		/* We only know how to deal with v2 EA blocks */
		header = (struct ext2_ext_attr_header *) block_buf;
		if (header->h_magic != EXT2_EXT_ATTR_MAGIC) {
			err = EXT2_ET_BAD_EA_HEADER;
			goto out3;
		}

		/* Read EAs */
		storage_size = handle->fs->blocksize -
			sizeof(struct ext2_ext_attr_header);
		start = block_buf + sizeof(struct ext2_ext_attr_header);
		err = read_xattrs_from_buffer(handle, inode,
					(struct ext2_ext_attr_entry *) start,
					storage_size, block_buf);
	}

out3:
	if (block_buf)
		ext2fs_free_mem(&block_buf);
out:
	return err;
}

errcode_t ext2fs_xattrs_read(struct ext2_xattr_handle *handle)
{
	struct ext2_inode_large *inode;
	size_t inode_size = EXT2_INODE_SIZE(handle->fs->super);
	errcode_t err;

	EXT2_CHECK_MAGIC(handle, EXT2_ET_MAGIC_EA_HANDLE);

	if (inode_size < sizeof(*inode))
		inode_size = sizeof(*inode);
	err = ext2fs_get_memzero(inode_size, &inode);
	if (err)
		return err;

	err = ext2fs_read_inode_full(handle->fs, handle->ino, EXT2_INODE(inode),
				     EXT2_INODE_SIZE(handle->fs->super));
	if (err)
		goto out;

	err = ext2fs_xattrs_read_inode(handle, inode);

out:
	ext2fs_free_mem(&inode);

	return err;
}

errcode_t ext2fs_xattrs_iterate(struct ext2_xattr_handle *h,
				int (*func)(char *name, char *value,
					    size_t value_len,
					    ext2_ino_t inode_num, void *data),
				void *data)
{
	struct ext2_xattr *x;
	int dirty = 0;
	int ret;

	EXT2_CHECK_MAGIC(h, EXT2_ET_MAGIC_EA_HANDLE);
	for (x = h->attrs; x < h->attrs + h->count; x++) {
		ret = func(x->name, x->value, x->value_len, x->ea_ino, data);
		if (ret & XATTR_CHANGED)
			dirty = 1;
		if (ret & XATTR_ABORT)
			break;
	}

	if (dirty)
		return ext2fs_xattrs_write(h);
	return 0;
}

errcode_t ext2fs_xattr_get(struct ext2_xattr_handle *h, const char *key,
			   void **value, size_t *value_len)
{
	struct ext2_xattr *x;
	char *val;
	errcode_t err;

	EXT2_CHECK_MAGIC(h, EXT2_ET_MAGIC_EA_HANDLE);
	for (x = h->attrs; x < h->attrs + h->count; x++) {
		if (strcmp(x->name, key))
			continue;

		if (!(h->flags & XATTR_HANDLE_FLAG_RAW) &&
		    ((strcmp(key, "system.posix_acl_default") == 0) ||
		     (strcmp(key, "system.posix_acl_access") == 0))) {
			err = convert_disk_buffer_to_posix_acl(x->value, x->value_len,
							       value, value_len);
			return err;
		} else {
			err = ext2fs_get_mem(x->value_len, &val);
			if (err)
				return err;
			memcpy(val, x->value, x->value_len);
			*value = val;
			*value_len = x->value_len;
			return 0;
		}
	}

	return EXT2_ET_EA_KEY_NOT_FOUND;
}

errcode_t ext2fs_xattr_inode_max_size(ext2_filsys fs, ext2_ino_t ino,
				      size_t *size)
{
	struct ext2_ext_attr_entry *entry;
	struct ext2_inode_large *inode;
	__u32 ea_inode_magic;
	unsigned int minoff;
	char *start;
	size_t i;
	errcode_t err;

	i = EXT2_INODE_SIZE(fs->super);
	if (i < sizeof(*inode))
		i = sizeof(*inode);
	err = ext2fs_get_memzero(i, &inode);
	if (err)
		return err;

	err = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)inode,
				     EXT2_INODE_SIZE(fs->super));
	if (err)
		goto out;

	/* Does the inode have size for EA? */
	if (EXT2_INODE_SIZE(fs->super) <= EXT2_GOOD_OLD_INODE_SIZE +
						  inode->i_extra_isize +
						  sizeof(__u32)) {
		err = EXT2_ET_INLINE_DATA_NO_SPACE;
		goto out;
	}

	minoff = EXT2_INODE_SIZE(fs->super) - sizeof(*inode) - sizeof(__u32);
	memcpy(&ea_inode_magic, ((char *) inode) + EXT2_GOOD_OLD_INODE_SIZE +
	       inode->i_extra_isize, sizeof(__u32));
	if (ea_inode_magic == EXT2_EXT_ATTR_MAGIC) {
		/* has xattrs.  calculate the size */
		start= ((char *) inode) + EXT2_GOOD_OLD_INODE_SIZE +
			inode->i_extra_isize + sizeof(__u32);
		entry = (struct ext2_ext_attr_entry *) start;
		while (!EXT2_EXT_IS_LAST_ENTRY(entry)) {
			if (!entry->e_value_inum && entry->e_value_size) {
				unsigned int offs = entry->e_value_offs;
				if (offs < minoff)
					minoff = offs;
			}
			entry = EXT2_EXT_ATTR_NEXT(entry);
		}
		*size = minoff - ((char *)entry - (char *)start) - sizeof(__u32);
	} else {
		/* no xattr.  return a maximum size */
		*size = EXT2_EXT_ATTR_SIZE(minoff -
					   EXT2_EXT_ATTR_LEN(strlen("data")) -
					   EXT2_EXT_ATTR_ROUND - sizeof(__u32));
	}

out:
	ext2fs_free_mem(&inode);
	return err;
}

static errcode_t xattr_create_ea_inode(ext2_filsys fs, const void *value,
				       size_t value_len, ext2_ino_t *ea_ino)
{
	struct ext2_inode inode;
	ext2_ino_t ino;
	ext2_file_t file;
	__u32 hash;
	errcode_t ret;

	ret = ext2fs_new_inode(fs, 0, 0, 0, &ino);
	if (ret)
		return ret;

	memset(&inode, 0, sizeof(inode));
	inode.i_flags |= EXT4_EA_INODE_FL;
	if (ext2fs_has_feature_extents(fs->super))
		inode.i_flags |= EXT4_EXTENTS_FL;
	inode.i_size = 0;
	inode.i_mode = LINUX_S_IFREG | 0600;
	inode.i_links_count = 1;
	ret = ext2fs_write_new_inode(fs, ino, &inode);
	if (ret)
		return ret;
	/*
	 * ref_count and hash utilize inode's i_*time fields.
	 * ext2fs_write_new_inode() call above initializes these fields with
	 * current time. That's why ref count and hash updates are done
	 * separately below.
	 */
	ext2fs_set_ea_inode_ref(&inode, 1);
	hash = ext2fs_crc32c_le(fs->csum_seed, value, value_len);
	ext2fs_set_ea_inode_hash(&inode, hash);

	ret = ext2fs_write_inode(fs, ino, &inode);
	if (ret)
		return ret;

	ret = ext2fs_file_open(fs, ino, EXT2_FILE_WRITE, &file);
	if (ret)
		return ret;
	ret = ext2fs_file_write(file, value, value_len, NULL);
	ext2fs_file_close(file);
	if (ret)
		return ret;

	ext2fs_inode_alloc_stats2(fs, ino, 1 /* inuse */, 0 /* isdir */);

	*ea_ino = ino;
	return 0;
}

static errcode_t xattr_inode_dec_ref(ext2_filsys fs, ext2_ino_t ino)
{
	struct ext2_inode_large inode;
	__u64 ref_count;
	errcode_t ret;

	ret = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)&inode,
				     sizeof(inode));
	if (ret)
		goto out;

	ref_count = ext2fs_get_ea_inode_ref(EXT2_INODE(&inode));
	ref_count--;
	ext2fs_set_ea_inode_ref(EXT2_INODE(&inode), ref_count);

	if (ref_count)
		goto write_out;

	inode.i_links_count = 0;
	inode.i_dtime = fs->now ? fs->now : time(0);

	ret = ext2fs_free_ext_attr(fs, ino, &inode);
	if (ret)
		goto write_out;

	if (ext2fs_inode_has_valid_blocks2(fs, (struct ext2_inode *)&inode)) {
		ret = ext2fs_punch(fs, ino, (struct ext2_inode *)&inode, NULL,
				   0, ~0ULL);
		if (ret)
			goto out;
	}

	ext2fs_inode_alloc_stats2(fs, ino, -1 /* inuse */, 0 /* is_dir */);

write_out:
	ret = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)&inode,
				      sizeof(inode));
out:
	return ret;
}

static errcode_t xattr_update_entry(ext2_filsys fs, struct ext2_xattr *x,
				    const char *name, const char *short_name,
				    int index, const void *value,
				    size_t value_len, int in_inode)
{
	ext2_ino_t ea_ino = 0;
	void *new_value = NULL;
	char *new_name = NULL;
	int name_len;
	errcode_t ret;

	if (!x->name) {
		name_len = strlen(name);
		ret = ext2fs_get_mem(name_len + 1, &new_name);
		if (ret)
			goto fail;
		memcpy(new_name, name, name_len + 1);
	}

	ret = ext2fs_get_mem(value_len, &new_value);
	if (ret)
		goto fail;
	memcpy(new_value, value, value_len);

	if (in_inode) {
		ret = xattr_create_ea_inode(fs, value, value_len, &ea_ino);
		if (ret)
			goto fail;
	}

	if (x->ea_ino) {
		ret = xattr_inode_dec_ref(fs, x->ea_ino);
		if (ret)
			goto fail;
	}

	if (!x->name) {
		x->name = new_name;
		x->short_name = new_name + (short_name  - name);
	}
	x->name_index = index;

	if (x->value)
		ext2fs_free_mem(&x->value);
	x->value = new_value;
	x->value_len = value_len;
	x->ea_ino = ea_ino;
	return 0;
fail:
	if (new_name)
		ext2fs_free_mem(&new_name);
	if (new_value)
		ext2fs_free_mem(&new_value);
	if (ea_ino)
		xattr_inode_dec_ref(fs, ea_ino);
	return ret;
}

static int xattr_find_position(struct ext2_xattr *attrs, int count,
			       const char *shortname, int name_idx)
{
	struct ext2_xattr *x;
	int i;
	int shortname_len, x_shortname_len;

	shortname_len = strlen(shortname);

	for (i = 0, x = attrs; i < count; i++, x++) {
		if (name_idx < x->name_index)
			break;
		if (name_idx > x->name_index)
			continue;

		x_shortname_len = strlen(x->short_name);
		if (shortname_len < x_shortname_len)
			break;
		if (shortname_len > x_shortname_len)
			continue;

		if (memcmp(shortname, x->short_name, shortname_len) <= 0)
			break;
	}
	return i;
}

static errcode_t xattr_array_update(struct ext2_xattr_handle *h,
				    const char *name,
				    const void *value, size_t value_len,
				    int ibody_free, int block_free,
				    int old_idx, int in_inode)
{
	struct ext2_xattr tmp;
	int add_to_ibody;
	int needed;
	int name_len, name_idx = 0;
	const char *shortname = name;
	int new_idx;
	int ret;

	find_ea_index(name, &shortname, &name_idx);
	name_len = strlen(shortname);

	needed = EXT2_EXT_ATTR_LEN(name_len);
	if (!in_inode)
		needed += EXT2_EXT_ATTR_SIZE(value_len);

	if (old_idx >= 0 && old_idx < h->ibody_count) {
		ibody_free += EXT2_EXT_ATTR_LEN(name_len);
		if (!h->attrs[old_idx].ea_ino)
			ibody_free += EXT2_EXT_ATTR_SIZE(
						h->attrs[old_idx].value_len);
	}

	if (needed <= ibody_free) {
		if (old_idx < 0) {
			new_idx = h->ibody_count;
			add_to_ibody = 1;
			goto add_new;
		}

		/* Update the existing entry. */
		ret = xattr_update_entry(h->fs, &h->attrs[old_idx], name,
					 shortname, name_idx, value,
					 value_len, in_inode);
		if (ret)
			return ret;
		if (h->ibody_count <= old_idx) {
			/* Move entry from block to the end of ibody. */
			tmp = h->attrs[old_idx];
			memmove(h->attrs + h->ibody_count + 1,
				h->attrs + h->ibody_count,
				(old_idx - h->ibody_count) * sizeof(*h->attrs));
			h->attrs[h->ibody_count] = tmp;
			h->ibody_count++;
		}
		return 0;
	}

	if (h->ibody_count <= old_idx) {
		block_free += EXT2_EXT_ATTR_LEN(name_len);
		if (!h->attrs[old_idx].ea_ino)
			block_free +=
				EXT2_EXT_ATTR_SIZE(h->attrs[old_idx].value_len);
	}

	if (needed > block_free)
		return EXT2_ET_EA_NO_SPACE;

	if (old_idx >= 0) {
		/* Update the existing entry. */
		ret = xattr_update_entry(h->fs, &h->attrs[old_idx], name,
					 shortname, name_idx, value,
					 value_len, in_inode);
		if (ret)
			return ret;
		if (old_idx < h->ibody_count) {
			/*
			 * Move entry from ibody to the block. Note that
			 * entries in the block are sorted.
			 */
			new_idx = xattr_find_position(h->attrs + h->ibody_count,
						      h->count - h->ibody_count,
						      shortname, name_idx);
			new_idx += h->ibody_count - 1;
			tmp = h->attrs[old_idx];
			memmove(h->attrs + old_idx, h->attrs + old_idx + 1,
				(new_idx - old_idx) * sizeof(*h->attrs));
			h->attrs[new_idx] = tmp;
			h->ibody_count--;
		}
		return 0;
	}

	new_idx = xattr_find_position(h->attrs + h->ibody_count,
				      h->count - h->ibody_count,
				      shortname, name_idx);
	new_idx += h->ibody_count;
	add_to_ibody = 0;

add_new:
	if (h->count == h->capacity) {
		ret = ext2fs_xattrs_expand(h, 4);
		if (ret)
			return ret;
	}

	ret = xattr_update_entry(h->fs, &h->attrs[h->count], name, shortname,
				 name_idx, value, value_len, in_inode);
	if (ret)
		return ret;

	tmp = h->attrs[h->count];
	memmove(h->attrs + new_idx + 1, h->attrs + new_idx,
		(h->count - new_idx)*sizeof(*h->attrs));
	h->attrs[new_idx] = tmp;
	if (add_to_ibody)
		h->ibody_count++;
	h->count++;
	return 0;
}

static int space_used(struct ext2_xattr *attrs, int count)
{
	int total = 0;
	struct ext2_xattr *x;
	int i, len;

	for (i = 0, x = attrs; i < count; i++, x++) {
		len = strlen(x->short_name);
		total += EXT2_EXT_ATTR_LEN(len);
		if (!x->ea_ino)
			total += EXT2_EXT_ATTR_SIZE(x->value_len);
	}
	return total;
}

/*
 * The minimum size of EA value when you start storing it in an external inode
 * size of block - size of header - size of 1 entry - 4 null bytes
 */
#define EXT4_XATTR_MIN_LARGE_EA_SIZE(b)	\
	((b) - EXT2_EXT_ATTR_LEN(3) - sizeof(struct ext2_ext_attr_header) - 4)

errcode_t ext2fs_xattr_set(struct ext2_xattr_handle *h,
			   const char *name,
			   const void *value,
			   size_t value_len)
{
	ext2_filsys fs = h->fs;
	const int inode_size = EXT2_INODE_SIZE(fs->super);
	struct ext2_inode_large *inode = NULL;
	struct ext2_xattr *x;
	char *new_value;
	int ibody_free, block_free;
	int in_inode = 0;
	int old_idx = -1;
	int extra_isize;
	errcode_t ret;

	EXT2_CHECK_MAGIC(h, EXT2_ET_MAGIC_EA_HANDLE);

	ret = ext2fs_get_mem(value_len, &new_value);
	if (ret)
		return ret;
	if (!(h->flags & XATTR_HANDLE_FLAG_RAW) &&
	    ((strcmp(name, "system.posix_acl_default") == 0) ||
	     (strcmp(name, "system.posix_acl_access") == 0))) {
		ret = convert_posix_acl_to_disk_buffer(value, value_len,
						       new_value, &value_len);
		if (ret)
			goto out;
	} else if (value_len)
		memcpy(new_value, value, value_len);

	/* Imitate kernel behavior by skipping update if value is the same. */
	for (x = h->attrs; x < h->attrs + h->count; x++) {
		if (!strcmp(x->name, name)) {
			if (!x->ea_ino && x->value_len == value_len &&
			    (!value_len ||
			     !memcmp(x->value, new_value, value_len))) {
				ret = 0;
				goto out;
			}
			old_idx = x - h->attrs;
			break;
		}
	}

	ret = ext2fs_get_memzero(inode_size, &inode);
	if (ret)
		goto out;
	ret = ext2fs_read_inode_full(fs, h->ino,
				     (struct ext2_inode *)inode,
				     inode_size);
	if (ret)
		goto out;
	if (inode_size > EXT2_GOOD_OLD_INODE_SIZE) {
		extra_isize = inode->i_extra_isize;
		if (extra_isize == 0) {
			extra_isize = fs->super->s_want_extra_isize;
			if (extra_isize == 0)
				extra_isize = sizeof(__u32);
		}
		ibody_free = inode_size - EXT2_GOOD_OLD_INODE_SIZE;
		ibody_free -= extra_isize;
		/* Extended attribute magic and final null entry. */
		ibody_free -= sizeof(__u32) * 2;
		ibody_free -= space_used(h->attrs, h->ibody_count);
	} else
		ibody_free = 0;

	/* Inline data can only go to ibody. */
	if (strcmp(name, "system.data") == 0) {
		if (h->ibody_count <= old_idx) {
			ret = EXT2_ET_FILESYSTEM_CORRUPTED;
			goto out;
		}
		ret = xattr_array_update(h, name, new_value, value_len,
					 ibody_free,
					 0 /* block_free */, old_idx,
					 0 /* in_inode */);
		if (ret)
			goto out;
		goto write_out;
	}

	block_free = fs->blocksize;
	block_free -= sizeof(struct ext2_ext_attr_header);
	/* Final null entry. */
	block_free -= sizeof(__u32);
	block_free -= space_used(h->attrs + h->ibody_count,
				 h->count - h->ibody_count);

	if (ext2fs_has_feature_ea_inode(fs->super) &&
	    value_len > EXT4_XATTR_MIN_LARGE_EA_SIZE(fs->blocksize))
		in_inode = 1;

	ret = xattr_array_update(h, name, new_value, value_len, ibody_free,
				 block_free, old_idx, in_inode);
	if (ret == EXT2_ET_EA_NO_SPACE && !in_inode &&
	    ext2fs_has_feature_ea_inode(fs->super))
		ret = xattr_array_update(h, name, new_value, value_len,
			ibody_free, block_free, old_idx, 1 /* in_inode */);
	if (ret)
		goto out;

write_out:
	ret = ext2fs_xattrs_write(h);
out:
	if (inode)
		ext2fs_free_mem(&inode);
	ext2fs_free_mem(&new_value);
	return ret;
}

errcode_t ext2fs_xattr_remove(struct ext2_xattr_handle *handle,
			      const char *key)
{
	struct ext2_xattr *x;
	struct ext2_xattr *end = handle->attrs + handle->count;

	EXT2_CHECK_MAGIC(handle, EXT2_ET_MAGIC_EA_HANDLE);
	for (x = handle->attrs; x < end; x++) {
		if (strcmp(x->name, key) == 0) {
			ext2fs_free_mem(&x->name);
			ext2fs_free_mem(&x->value);
			if (x->ea_ino)
				xattr_inode_dec_ref(handle->fs, x->ea_ino);
			memmove(x, x + 1, (end - x - 1)*sizeof(*x));
			memset(end - 1, 0, sizeof(*end));
			if (x < handle->attrs + handle->ibody_count)
				handle->ibody_count--;
			handle->count--;
			return ext2fs_xattrs_write(handle);
		}
	}

	/* no key found, success! */
	return 0;
}

errcode_t ext2fs_xattrs_open(ext2_filsys fs, ext2_ino_t ino,
			     struct ext2_xattr_handle **handle)
{
	struct ext2_xattr_handle *h;
	errcode_t err;

	if (!ext2fs_has_feature_xattr(fs->super) &&
	    !ext2fs_has_feature_inline_data(fs->super))
		return EXT2_ET_MISSING_EA_FEATURE;

	err = ext2fs_get_memzero(sizeof(*h), &h);
	if (err)
		return err;

	h->magic = EXT2_ET_MAGIC_EA_HANDLE;
	h->capacity = 4;
	err = ext2fs_get_arrayzero(h->capacity, sizeof(struct ext2_xattr),
				   &h->attrs);
	if (err) {
		ext2fs_free_mem(&h);
		return err;
	}
	h->count = 0;
	h->ino = ino;
	h->fs = fs;
	*handle = h;
	return 0;
}

errcode_t ext2fs_xattrs_close(struct ext2_xattr_handle **handle)
{
	struct ext2_xattr_handle *h = *handle;

	EXT2_CHECK_MAGIC(h, EXT2_ET_MAGIC_EA_HANDLE);
	xattrs_free_keys(h);
	ext2fs_free_mem(&h->attrs);
	ext2fs_free_mem(handle);
	return 0;
}

errcode_t ext2fs_xattrs_count(struct ext2_xattr_handle *handle, size_t *count)
{
	EXT2_CHECK_MAGIC(handle, EXT2_ET_MAGIC_EA_HANDLE);
	*count = handle->count;
	return 0;
}

errcode_t ext2fs_xattrs_flags(struct ext2_xattr_handle *handle,
			      unsigned int *new_flags, unsigned int *old_flags)
{
	EXT2_CHECK_MAGIC(handle, EXT2_ET_MAGIC_EA_HANDLE);
	if (old_flags)
		*old_flags = handle->flags;
	if (new_flags)
		handle->flags = *new_flags;
	return 0;
}

struct ext2_attr_info {
	int name_index;
	const char *name;
	const char *value;
	int value_len;
};

struct ext2_attr_search {
	struct ext2_ext_attr_entry *first;
	char *base;
	char *end;
	struct ext2_ext_attr_entry *here;
	int not_found;
};

struct ext2_attr_ibody_find {
	ext2_ino_t ino;
	struct ext2_attr_search s;
};

struct ext2_attr_block_find {
	struct ext2_attr_search s;
	char *block;
};

void ext2fs_attr_shift_entries(struct ext2_ext_attr_entry *entry,
			       int value_offs_shift, char *to,
			       char *from, int n)
{
	struct ext2_ext_attr_entry *last = entry;

	/* Adjust the value offsets of the entries */
	for (; !EXT2_EXT_IS_LAST_ENTRY(last); last = EXT2_EXT_ATTR_NEXT(last)) {
		if (!last->e_value_inum && last->e_value_size) {
			last->e_value_offs = last->e_value_offs +
							value_offs_shift;
		}
	}
	/* Shift the entries by n bytes and zero freed space in inode */
	memmove(to, from, n);
	if (to > from)
		memset(from, 0, to - from);
}

/*
 * This function returns the free space present in the inode or the EA block.
 * total is number of bytes taken up by the EA entries and is used to shift
 * the EAs in ext2fs_expand_extra_isize().
 */
int ext2fs_attr_free_space(struct ext2_ext_attr_entry *last,
			   int *min_offs, char *base, int *total)
{
	for (; !EXT2_EXT_IS_LAST_ENTRY(last); last = EXT2_EXT_ATTR_NEXT(last)) {
		*total += EXT2_EXT_ATTR_LEN(last->e_name_len);
		if (!last->e_value_inum && last->e_value_size) {
			int offs = last->e_value_offs;
			if (offs < *min_offs)
				*min_offs = offs;
		}
	}

	return *min_offs - ((char *)last - base) - sizeof(__u32);
}

static errcode_t ext2fs_attr_check_names(struct ext2_ext_attr_entry *entry,
					 char *end)
{
	while (!EXT2_EXT_IS_LAST_ENTRY(entry)) {
		struct ext2_ext_attr_entry *next = EXT2_EXT_ATTR_NEXT(entry);
		if ((char *)next >= end)
			return EXT2_ET_EA_BAD_ENTRIES;
		entry = next;
	}
	return 0;
}

/* The unused parameter used to be the blocksize, but with in-inode xattrs
 * the xattr storage area size depends on where the xattrs are kept.  Keep
 * this parameter for API/ABI compatibility, but it is not needed. */
static errcode_t ext2fs_attr_find_entry(struct ext2_ext_attr_entry **pentry,
					int name_index, const char *name,
					int unused, int sorted)
{
	struct ext2_ext_attr_entry *entry;
	int name_len;
	int cmp = 1;

	if (name == NULL)
		return EXT2_ET_EA_BAD_NAME;

	name_len = strlen(name);
	entry = *pentry;
	for (; !EXT2_EXT_IS_LAST_ENTRY(entry);
		entry = EXT2_EXT_ATTR_NEXT(entry)) {
		cmp = name_index - entry->e_name_index;
		if (!cmp)
			cmp = name_len - entry->e_name_len;
		if (!cmp)
			cmp = memcmp(name, entry->e_name, name_len);
		if (cmp <= 0 && (sorted || cmp == 0))
			break;
	}
	*pentry = entry;

	return cmp ? EXT2_ET_EA_NAME_NOT_FOUND : 0;
}

static errcode_t ext2fs_attr_block_find(ext2_filsys fs,struct ext2_inode *inode,
					struct ext2_attr_info *i,
					struct ext2_attr_block_find *bs)
{
	struct ext2_ext_attr_header *header;
	errcode_t error;

	if (inode->i_file_acl) {
		/* The inode already has an extended attribute block. */
		error = ext2fs_get_mem(fs->blocksize, &bs->block);
		if (error)
			return error;
		error = ext2fs_read_ext_attr(fs, inode->i_file_acl, bs->block);
		if (error)
			goto cleanup;

		header = BHDR(bs->block);
		if (header->h_magic != EXT2_EXT_ATTR_MAGIC) {
			error = EXT2_ET_EA_BAD_MAGIC;
			goto cleanup;
		}

		/* Find the named attribute. */
		bs->s.base = bs->block;
		bs->s.first = (struct ext2_ext_attr_entry *)(header + 1);
		bs->s.end = bs->block + fs->blocksize;
		bs->s.here = bs->s.first;
		error = ext2fs_attr_find_entry(&bs->s.here, i->name_index,
					       i->name, fs->blocksize, 1);
		if (error && error != EXT2_ET_EA_NAME_NOT_FOUND)
			goto cleanup;
		bs->s.not_found = error;
	}
	error = 0;

cleanup:
	if (error && bs->block)
		ext2fs_free_mem(&bs->block);
	return error;
}

static errcode_t ext2fs_attr_ibody_find(ext2_filsys fs,
					struct ext2_inode_large *inode,
					struct ext2_attr_info *i,
					struct ext2_attr_ibody_find *is)
{
	errcode_t error;

	if (EXT2_INODE_SIZE(fs->super) == EXT2_GOOD_OLD_INODE_SIZE)
		return 0;

	if (inode->i_extra_isize == 0)
		return 0;

	is->s.first = &IHDR(inode)->h_first_entry[0];
	is->s.base = (char *)is->s.first;
	is->s.here = is->s.first;
	is->s.end = (char *)inode + EXT2_INODE_SIZE(fs->super);
	if (IHDR(inode)->h_magic == EXT2_EXT_ATTR_MAGIC) {
		error = ext2fs_attr_check_names(is->s.first, is->s.end);
		if (error)
			return error;
		/* Find the named attribute. */
		error = ext2fs_attr_find_entry(&is->s.here, i->name_index,
					       i->name, is->s.end -
					       (char *)is->s.base, 0);
		if (error && error != EXT2_ET_EA_NAME_NOT_FOUND)
			return error;
		is->s.not_found = error;
	}

	return 0;
}

static errcode_t ext2fs_attr_set_entry(ext2_filsys fs, struct ext2_attr_info *i,
				       struct ext2_attr_search *s)
{
	struct ext2_ext_attr_entry *last;
	int free, min_offs = s->end - s->base, name_len = strlen(i->name);

	/* Compute min_offs and last. */
	for (last = s->first; !EXT2_EXT_IS_LAST_ENTRY(last);
	     last = EXT2_EXT_ATTR_NEXT(last)) {
		if (!last->e_value_inum && last->e_value_size) {
			int offs = last->e_value_offs;

			if (offs < min_offs)
				min_offs = offs;
		}
	}
	free = min_offs - ((char *)last - s->base) - sizeof(__u32);

	if (!s->not_found) {
		if (!s->here->e_value_inum && s->here->e_value_size) {
			int size = s->here->e_value_size;
			free += EXT2_EXT_ATTR_SIZE(size);
		}
		free += EXT2_EXT_ATTR_LEN(name_len);
	}
	if (i->value) {
		if (free < EXT2_EXT_ATTR_LEN(name_len) +
			   EXT2_EXT_ATTR_SIZE(i->value_len))
			return EXT2_ET_EA_NO_SPACE;
	}

	if (i->value && s->not_found) {
		/* Insert the new name. */
		int size = EXT2_EXT_ATTR_LEN(name_len);
		int rest = (char *)last - (char *)s->here + sizeof(__u32);

		memmove((char *)s->here + size, s->here, rest);
		memset(s->here, 0, size);
		s->here->e_name_index = i->name_index;
		s->here->e_name_len = name_len;
		memcpy(s->here->e_name, i->name, name_len);
	} else {
		if (!s->here->e_value_inum && s->here->e_value_size) {
			char *first_val = s->base + min_offs;
			int offs = s->here->e_value_offs;
			char *val = s->base + offs;
			int size = EXT2_EXT_ATTR_SIZE(s->here->e_value_size);

			if (i->value &&
			    size == EXT2_EXT_ATTR_SIZE(i->value_len)) {
				/* The old and the new value have the same
				   size. Just replace. */
				s->here->e_value_size = i->value_len;
				memset(val + size - EXT2_EXT_ATTR_PAD, 0,
				       EXT2_EXT_ATTR_PAD); /* Clear pad bytes */
				memcpy(val, i->value, i->value_len);
				return 0;
			}

			/* Remove the old value. */
			memmove(first_val + size, first_val, val - first_val);
			memset(first_val, 0, size);
			s->here->e_value_size = 0;
			s->here->e_value_offs = 0;
			min_offs += size;

			/* Adjust all value offsets. */
			last = s->first;
			while (!EXT2_EXT_IS_LAST_ENTRY(last)) {
				int o = last->e_value_offs;

				if (!last->e_value_inum &&
				    last->e_value_size && o < offs)
					last->e_value_offs = o + size;
				last = EXT2_EXT_ATTR_NEXT(last);
			}
		}
		if (!i->value) {
			/* Remove the old name. */
			int size = EXT2_EXT_ATTR_LEN(name_len);

			last = ENTRY((char *)last - size);
			memmove((char *)s->here, (char *)s->here + size,
				(char *)last - (char *)s->here + sizeof(__u32));
			memset(last, 0, size);
		}
	}

	if (i->value) {
		/* Insert the new value. */
		s->here->e_value_size = i->value_len;
		if (i->value_len) {
			int size = EXT2_EXT_ATTR_SIZE(i->value_len);
			char *val = s->base + min_offs - size;

			s->here->e_value_offs = min_offs - size;
			memset(val + size - EXT2_EXT_ATTR_PAD, 0,
			       EXT2_EXT_ATTR_PAD); /* Clear the pad bytes. */
			memcpy(val, i->value, i->value_len);
		}
	}

	return 0;
}

static errcode_t ext2fs_attr_block_set(ext2_filsys fs, struct ext2_inode *inode,
				       struct ext2_attr_info *i,
				       struct ext2_attr_block_find *bs)
{
	struct ext2_attr_search *s = &bs->s;
	char *new_buf = NULL, *old_block = NULL;
	blk_t blk;
	int clear_flag = 0;
	errcode_t error;

	if (i->value && i->value_len > fs->blocksize)
		return EXT2_ET_EA_NO_SPACE;

	if (s->base) {
		if (BHDR(s->base)->h_refcount != 1) {
			int offset = (char *)s->here - bs->block;

			/* Decrement the refcount of the shared block */
			old_block = s->base;
			BHDR(s->base)->h_refcount -= 1;

			error = ext2fs_get_mem(fs->blocksize, &s->base);
			if (error)
				goto cleanup;
			clear_flag = 1;
			memcpy(s->base, bs->block, fs->blocksize);
			s->first = ENTRY(BHDR(s->base)+1);
			BHDR(s->base)->h_refcount = 1;
			s->here = ENTRY(s->base + offset);
			s->end = s->base + fs->blocksize;
		}
	} else {
		error = ext2fs_get_mem(fs->blocksize, &s->base);
		if (error)
			goto cleanup;
		clear_flag = 1;
		memset(s->base, 0, fs->blocksize);
		BHDR(s->base)->h_magic = EXT2_EXT_ATTR_MAGIC;
		BHDR(s->base)->h_blocks = 1;
		BHDR(s->base)->h_refcount = 1;
		s->first = ENTRY(BHDR(s->base)+1);
		s->here = ENTRY(BHDR(s->base)+1);
		s->end = s->base + fs->blocksize;
	}

	error = ext2fs_attr_set_entry(fs, i, s);
	if (error)
		goto cleanup;

	if (!EXT2_EXT_IS_LAST_ENTRY(s->first))
		ext2fs_attr_rehash(BHDR(s->base), s->here);

	if (!EXT2_EXT_IS_LAST_ENTRY(s->first)) {
		if (bs->block && bs->block == s->base) {
			/* We are modifying this block in-place */
			new_buf = bs->block;
			blk = inode->i_file_acl;
			error = ext2fs_write_ext_attr(fs, blk, s->base);
			if (error)
				goto cleanup;
		} else {
			/* We need to allocate a new block */
			error = ext2fs_new_block(fs, 0, 0, &blk);
			if (error)
				goto cleanup;
			ext2fs_block_alloc_stats(fs, blk, 1);
			error = ext2fs_write_ext_attr(fs, blk, s->base);
			if (error)
				goto cleanup;
			new_buf = s->base;
			if (old_block) {
				BHDR(s->base)->h_refcount -= 1;
				error = ext2fs_write_ext_attr(fs,
							      inode->i_file_acl,
							      s->base);
				if (error)
					goto cleanup;
			}
		}
	}

	/* Update the i_blocks if we added a new EA block */
	if (!inode->i_file_acl && new_buf)
		inode->i_blocks += fs->blocksize / 512;
	/* Update the inode. */
	inode->i_file_acl = new_buf ? blk : 0;

cleanup:
	if (clear_flag)
		ext2fs_free_mem(&s->base);
	return 0;
}

static errcode_t ext2fs_attr_ibody_set(ext2_filsys fs,
				       struct ext2_inode_large *inode,
				       struct ext2_attr_info *i,
				       struct ext2_attr_ibody_find *is)
{
	struct ext2_attr_search *s = &is->s;
	errcode_t error;

	if (EXT2_INODE_SIZE(fs->super) == EXT2_GOOD_OLD_INODE_SIZE)
		return EXT2_ET_EA_NO_SPACE;

	error = ext2fs_attr_set_entry(fs, i, s);
	if (error)
		return error;

	if (!EXT2_EXT_IS_LAST_ENTRY(s->first))
		IHDR(inode)->h_magic = EXT2_EXT_ATTR_MAGIC;
	else
		IHDR(inode)->h_magic = 0;

	return ext2fs_write_inode_full(fs, is->ino, (struct ext2_inode *)inode,
				       EXT2_INODE_SIZE(fs->super));
}

static struct {
	char str[28];
	int len;
} ext2_attr_index_prefix[] = {
	[EXT2_ATTR_INDEX_USER] = { EXT2_ATTR_INDEX_USER_PREFIX,
				   sizeof(EXT2_ATTR_INDEX_USER_PREFIX) },
	[EXT2_ATTR_INDEX_POSIX_ACL_ACCESS] = {
		EXT2_ATTR_INDEX_POSIX_ACL_ACCESS_PREFIX,
		sizeof(EXT2_ATTR_INDEX_POSIX_ACL_ACCESS_PREFIX) },
	[EXT2_ATTR_INDEX_POSIX_ACL_DEFAULT] = {
		EXT2_ATTR_INDEX_POSIX_ACL_DEFAULT_PREFIX,
		sizeof(EXT2_ATTR_INDEX_POSIX_ACL_DEFAULT_PREFIX) },
	[EXT2_ATTR_INDEX_TRUSTED] = { EXT2_ATTR_INDEX_TRUSTED_PREFIX,
				      sizeof(EXT2_ATTR_INDEX_TRUSTED_PREFIX) },
	[EXT2_ATTR_INDEX_LUSTRE] = { EXT2_ATTR_INDEX_LUSTRE_PREFIX,
				     sizeof(EXT2_ATTR_INDEX_LUSTRE_PREFIX) },
	[EXT2_ATTR_INDEX_SECURITY] = { EXT2_ATTR_INDEX_SECURITY_PREFIX,
				       sizeof(EXT2_ATTR_INDEX_SECURITY_PREFIX)},
	{ "", 0 }
};

errcode_t ext2fs_attr_set(ext2_filsys fs, ext2_ino_t ino,
			  struct ext2_inode *inode,
			  int name_index, const char *name, const char *value,
			  int value_len, int flags)
{
	struct ext2_inode_large *inode_large = NULL;
	struct ext2_attr_info i = {
		.name_index = name_index,
		.name = name,
		.value = value,
		.value_len = value_len,
	};
	struct ext2_attr_ibody_find is = {
		.ino = ino,
		.s = { .not_found = -ENODATA, },
	};
	struct ext2_attr_block_find bs = {
		.s = { .not_found = -ENODATA, },
	};
	errcode_t error;

	if (!name)
		return EXT2_ET_EA_BAD_NAME;
	if (strlen(name) > 255)
		return EXT2_ET_EA_NAME_TOO_BIG;

	/* If the prefix is still present, skip it */
	if (strncmp(name, ext2_attr_index_prefix[name_index].str,
		    ext2_attr_index_prefix[name_index].len) == 0)
		i.name += ext2_attr_index_prefix[name_index].len;

	if (EXT2_INODE_SIZE(fs->super) > EXT2_GOOD_OLD_INODE_SIZE) {
		inode_large = (struct ext2_inode_large *)inode;

		error = ext2fs_attr_ibody_find(fs, inode_large, &i, &is);
		if (error)
			goto cleanup;
	}
	if (is.s.not_found) {
		error = ext2fs_attr_block_find(fs, inode, &i, &bs);
		if (error)
			goto cleanup;
	}

	if (is.s.not_found && bs.s.not_found) {
		error = EXT2_ET_EA_NAME_NOT_FOUND;
		if (flags & XATTR_REPLACE)
			goto cleanup;
		error = 0;
		if (!value)
			goto cleanup;
	} else {
		error = EXT2_ET_EA_NAME_EXISTS;
		if (flags & XATTR_CREATE)
			goto cleanup;
	}

	if (!value) {
		if (!is.s.not_found &&
		    (EXT2_INODE_SIZE(fs->super) > EXT2_GOOD_OLD_INODE_SIZE))
			error = ext2fs_attr_ibody_set(fs, inode_large, &i, &is);
		else if (!bs.s.not_found)
			error = ext2fs_attr_block_set(fs, inode, &i, &bs);
	} else {
		if (EXT2_INODE_SIZE(fs->super) > EXT2_GOOD_OLD_INODE_SIZE)
			error = ext2fs_attr_ibody_set(fs, inode_large, &i, &is);
		if (!error && !bs.s.not_found) {
			i.value = NULL;
			error = ext2fs_attr_block_set(fs, inode, &i, &bs);
		} else if (error == EXT2_ET_EA_NO_SPACE) {
			error = ext2fs_attr_block_set(fs, inode, &i, &bs);
			if (error)
				goto cleanup;
			if (!is.s.not_found) {
				i.value = NULL;
				if (EXT2_INODE_SIZE(fs->super) >
				    EXT2_GOOD_OLD_INODE_SIZE)
					error = ext2fs_attr_ibody_set(fs,
							inode_large, &i, &is);
			}
		}
	}

cleanup:
	return error;
}

static errcode_t ext2fs_attr_check_block(ext2_filsys fs, char *buffer)
{
	if (BHDR(buffer)->h_magic != (EXT2_EXT_ATTR_MAGIC) ||
	    BHDR(buffer)->h_blocks != 1)
		return EXT2_ET_EA_BAD_MAGIC;

	return ext2fs_attr_check_names((struct ext2_ext_attr_entry *)
				       (BHDR(buffer) + 1),
				       buffer + fs->blocksize);
}

static errcode_t ext2fs_attr_block_get(ext2_filsys fs, struct ext2_inode *inode,
				       int name_index, const char *name,
				       void *buffer, size_t buffer_size,
				       int *easize)
{
	struct ext2_ext_attr_header *header = NULL;
	struct ext2_ext_attr_entry *entry;
	char *block_buf = NULL;
	errcode_t error;

	error = EXT2_ET_EA_NAME_NOT_FOUND;
	if (!inode->i_file_acl)
		goto cleanup;

	error = ext2fs_get_mem(fs->blocksize, &block_buf);
	if (error)
		return error;
	error = ext2fs_read_ext_attr(fs, inode->i_file_acl, block_buf);
	if (error)
		goto cleanup;

	error = ext2fs_attr_check_block(fs, block_buf);
	if (error)
		goto cleanup;

	header = BHDR(block_buf);
	entry = (struct ext2_ext_attr_entry *)(header + 1);
	error = ext2fs_attr_find_entry(&entry, name_index, name,
				       fs->blocksize, 1);
	if (error)
		goto cleanup;
	if (easize)
		*easize = entry->e_value_size;
	if (buffer) {
		if (entry->e_value_size > buffer_size) {
			error = EXT2_ET_EA_TOO_BIG;
			goto cleanup;
		}
		memcpy(buffer, block_buf + entry->e_value_offs,
		       entry->e_value_size);
		error = 0;
	}

cleanup:
	if (block_buf)
		ext2fs_free_mem(&block_buf);
	return error;
}

static errcode_t ext2fs_attr_check_ibody(ext2_filsys fs,
					 struct ext2_inode_large *inode)
{
	const int inode_size = EXT2_INODE_SIZE(fs->super);

	if (inode_size == EXT2_GOOD_OLD_INODE_SIZE)
		return EXT2_ET_EA_NAME_NOT_FOUND;

	if (IHDR(inode)->h_magic != EXT2_EXT_ATTR_MAGIC)
		return EXT2_ET_EA_BAD_MAGIC;

	return ext2fs_attr_check_names(&IHDR(inode)->h_first_entry[0],
				       (char *)inode + inode_size);
}


static errcode_t ext2fs_attr_ibody_get(ext2_filsys fs,
				       struct ext2_inode_large *inode,
				       int name_index, const char *name,
				       void *buffer, size_t buffer_size,
				       int *easize)
{
	struct ext2_ext_attr_entry *entry;
	int error;

	error = ext2fs_attr_check_ibody(fs, inode);
	if (error)
		return error;

	entry = &IHDR(inode)->h_first_entry[0];

	error = ext2fs_attr_find_entry(&entry, name_index, name,
				(char *)inode + EXT2_INODE_SIZE(fs->super) -
				(char *)entry, 0);
	if (error)
		goto cleanup;
	if (easize)
		*easize = entry->e_value_size;
	if (buffer) {
		if (entry->e_value_size > buffer_size) {
			error = EXT2_ET_EA_TOO_BIG;
			goto cleanup;
		}
		memcpy(buffer, (char *)&IHDR(inode)->h_first_entry[0] +
			       entry->e_value_offs, entry->e_value_size);
	}

cleanup:
	return error;
}


errcode_t ext2fs_attr_get(ext2_filsys fs, struct ext2_inode *inode,
			  int name_index, const char *name, char *buffer,
			  size_t buffer_size, int *easize)
{
	errcode_t error;

	error = ext2fs_attr_ibody_get(fs, (struct ext2_inode_large *)inode,
				      name_index, name, buffer, buffer_size,
				      easize);
	if (error == EXT2_ET_EA_NAME_NOT_FOUND || error == EXT2_ET_EA_BAD_MAGIC)
		error = ext2fs_attr_block_get(fs, inode, name_index, name,
					      buffer, buffer_size, easize);

	return error;
}

int ext2fs_attr_get_next_attr(struct ext2_ext_attr_entry *entry, int name_index,
			      char *buffer, int buffer_size, int start)
{
	const int prefix_len = ext2_attr_index_prefix[name_index].len;
	int total_len;

	if (!start && !EXT2_EXT_IS_LAST_ENTRY(entry))
		entry = EXT2_EXT_ATTR_NEXT(entry);

	for (; !EXT2_EXT_IS_LAST_ENTRY(entry);
	     entry = EXT2_EXT_ATTR_NEXT(entry)) {
		if (!name_index)
			break;
		if (name_index == entry->e_name_index)
			break;
	}
	if (EXT2_EXT_IS_LAST_ENTRY(entry))
		return 0;

	total_len = prefix_len + entry->e_name_len + 1;
	if (buffer && total_len <= buffer_size) {
		memcpy(buffer, ext2_attr_index_prefix[name_index].str,
		       prefix_len);
		memcpy(buffer + prefix_len, entry->e_name, entry->e_name_len);
		buffer[prefix_len + entry->e_name_len] = '\0';
	}

	return total_len;
}

errcode_t ext2fs_expand_extra_isize(ext2_filsys fs, ext2_ino_t ino,
				    struct ext2_inode_large *inode,
				    int new_extra_isize, int *ret,
				    int *needed_size)
{
	struct ext2_inode *inode_buf = NULL;
	struct ext2_ext_attr_header *header = NULL;
	struct ext2_ext_attr_entry *entry = NULL, *last = NULL;
	struct ext2_attr_ibody_find is = {
		.ino = ino,
		.s = { .not_found = EXT2_ET_EA_NO_SPACE, },
	};
	struct ext2_attr_block_find bs = {
		.s = { .not_found = EXT2_ET_EA_NO_SPACE, },
	};
	char *start, *end, *block_buf = NULL, *buffer =NULL, *b_entry_name=NULL;
	int total_ino = 0, total_blk, free, offs, tried_min_extra_isize = 0;
	int s_min_extra_isize = fs->super->s_min_extra_isize;
	errcode_t error = 0;

	if (needed_size)
		*needed_size = new_extra_isize;
	error = ext2fs_get_mem(fs->blocksize, &block_buf);
	if (error)
		return error;

	if (inode == NULL) {
		error = ext2fs_get_mem(EXT2_INODE_SIZE(fs->super), &inode_buf);
		if (error)
			goto cleanup;

		error = ext2fs_read_inode_full(fs, ino, inode_buf,
					       EXT2_INODE_SIZE(fs->super));
		if (error)
			goto cleanup;

		inode = (struct ext2_inode_large *)inode_buf;
	}

retry:
	if (inode->i_extra_isize >= new_extra_isize)
		goto cleanup;

	start = (char *)inode + EXT2_GOOD_OLD_INODE_SIZE + inode->i_extra_isize;
	/* No extended attributes present */
	if (IHDR(inode)->h_magic != EXT2_EXT_ATTR_MAGIC) {
		memset(start, 0,
		       EXT2_INODE_SIZE(fs->super) - EXT2_GOOD_OLD_INODE_SIZE -
		       inode->i_extra_isize);
		inode->i_extra_isize = new_extra_isize;
		if (needed_size)
			*needed_size = 0;
		goto write_inode;
	}

	start += sizeof(__u32);
	end = (char *)inode + EXT2_INODE_SIZE(fs->super);
	last = entry = (struct ext2_ext_attr_entry *)start;
	offs = end - start;
	/* Consider space takenup by magic number */
	total_ino = sizeof(__u32);
	free = ext2fs_attr_free_space(last, &offs, start, &total_ino);

	/* Enough free space available in the inode for expansion */
	if (free >= new_extra_isize) {
		ext2fs_attr_shift_entries(entry,
					inode->i_extra_isize - new_extra_isize,
					(char *)inode +
					EXT2_GOOD_OLD_INODE_SIZE +
					new_extra_isize,
					start - sizeof(__u32), total_ino);
		inode->i_extra_isize = new_extra_isize;
		if (needed_size)
			*needed_size = 0;
		goto write_inode;
	}

	if (inode->i_file_acl) {
		error = ext2fs_read_ext_attr(fs, inode->i_file_acl, block_buf);
		if (error)
			goto cleanup;

		header = BHDR(block_buf);
		if (header->h_magic != EXT2_EXT_ATTR_MAGIC) {
			error = EXT2_ET_EA_BAD_MAGIC;
			goto cleanup;
		}
		end = block_buf + fs->blocksize;
		last = entry = (struct ext2_ext_attr_entry *)(header+1);
		start = (char *)entry;
		offs = end - start;
		free = ext2fs_attr_free_space(last, &offs, start, &total_blk);
		if (free < new_extra_isize) {
			if (!tried_min_extra_isize && s_min_extra_isize) {
				tried_min_extra_isize++;
				new_extra_isize = s_min_extra_isize;
				goto retry;
			}
			if (ret)
				*ret = EXT2_EXPAND_EISIZE_NOSPC;
			error = EXT2_ET_EA_NO_SPACE;
			goto cleanup;
		}
	} else {
		if (ret && *ret == EXT2_EXPAND_EISIZE_UNSAFE) {
			*ret = EXT2_EXPAND_EISIZE_NEW_BLOCK;
			error = 0;
			goto cleanup;
		}
		free = fs->blocksize;
	}

	while (new_extra_isize > 0) {
		int offs, size, entry_size;
		struct ext2_ext_attr_entry *small_entry = NULL;
		struct ext2_attr_info i = {
			.value = NULL,
			.value_len = 0,
		};
		unsigned int total_size, shift_bytes, temp = ~0U, extra_isize=0;

		start = (char *)inode + EXT2_GOOD_OLD_INODE_SIZE +
				inode->i_extra_isize + sizeof(__u32);
		end = (char *)inode + EXT2_INODE_SIZE(fs->super);
		last = (struct ext2_ext_attr_entry *)start;

		/* Find the entry best suited to be pushed into EA block */
		entry = NULL;
		for (; !EXT2_EXT_IS_LAST_ENTRY(last);
			last = EXT2_EXT_ATTR_NEXT(last)) {
			total_size = EXT2_EXT_ATTR_SIZE(last->e_value_size) +
					EXT2_EXT_ATTR_LEN(last->e_name_len);
			if (total_size <= free && total_size < temp) {
				if (total_size < new_extra_isize) {
					small_entry = last;
				} else {
					entry = last;
					temp = total_size;
				}
			}
		}

		if (entry == NULL) {
			if (small_entry) {
				entry = small_entry;
			} else {
				if (!tried_min_extra_isize &&
				    s_min_extra_isize) {
					tried_min_extra_isize++;
					new_extra_isize = s_min_extra_isize;
					goto retry;
				}
				if (ret)
					*ret = EXT2_EXPAND_EISIZE_NOSPC;
				error = EXT2_ET_EA_NO_SPACE;
				goto cleanup;
			}
		}
		offs = entry->e_value_offs;
		size = entry->e_value_size;
		entry_size = EXT2_EXT_ATTR_LEN(entry->e_name_len);
		i.name_index = entry->e_name_index;
		error = ext2fs_get_mem(EXT2_EXT_ATTR_SIZE(size), &buffer);
		if (error)
			goto cleanup;
		error = ext2fs_get_mem(entry->e_name_len + 1, &b_entry_name);
		if (error)
			goto cleanup;
		/* Save the entry name and the entry value */
		memcpy((char *)buffer, (char *)start + offs,
		       EXT2_EXT_ATTR_SIZE(size));
		memcpy((char *)b_entry_name, (char *)entry->e_name,
		       entry->e_name_len);
		b_entry_name[entry->e_name_len] = '\0';
		i.name = b_entry_name;

		error = ext2fs_attr_ibody_find(fs, inode, &i, &is);
		if (error)
			goto cleanup;

		error = ext2fs_attr_set_entry(fs, &i, &is.s);
		if (error)
			goto cleanup;

		entry = (struct ext2_ext_attr_entry *)start;
		if (entry_size + EXT2_EXT_ATTR_SIZE(size) >= new_extra_isize)
			shift_bytes = new_extra_isize;
		else
			shift_bytes = entry_size + EXT2_EXT_ATTR_SIZE(size);
		ext2fs_attr_shift_entries(entry,
					inode->i_extra_isize - shift_bytes,
					(char *)inode +EXT2_GOOD_OLD_INODE_SIZE+
					extra_isize + shift_bytes,
					start - sizeof(__u32),
					total_ino - entry_size);

		extra_isize += shift_bytes;
		new_extra_isize -= shift_bytes;
		if (needed_size)
			*needed_size = new_extra_isize;
		inode->i_extra_isize = extra_isize;

		i.name = b_entry_name;
		i.value = buffer;
		i.value_len = size;
		error = ext2fs_attr_block_find(fs, (struct ext2_inode *)inode,
					       &i, &bs);
		if (error)
			goto cleanup;

		/* Add entry which was removed from the inode into the block */
		error = ext2fs_attr_block_set(fs, (struct ext2_inode *)inode,
					      &i, &bs);
		if (error)
			goto cleanup;
	}

write_inode:
	error = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)inode,
					EXT2_INODE_SIZE(fs->super));
cleanup:
	if (inode_buf)
		ext2fs_free_mem(&inode_buf);
	if (block_buf)
		ext2fs_free_mem(&block_buf);
	if (buffer)
		ext2fs_free_mem(&buffer);
	if (b_entry_name)
		ext2fs_free_mem(&b_entry_name);

	return error;
}
