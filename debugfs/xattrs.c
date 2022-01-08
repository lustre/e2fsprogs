/*
 * xattrs.c --- Modify extended attributes via debugfs.
 *
 * Copyright (C) 2014 Oracle.  This file may be redistributed
 * under the terms of the GNU Public License.
 */

#include "config.h"
#include <stdio.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
extern int optind;
extern char *optarg;
#endif
#include <ctype.h>
#include "support/cstring.h"

#include "debugfs.h"
#include "ext2fs/ext4_acl.h"
#include "ext2fs/lfsck.h"

#define PRINT_XATTR_HEX		0x01
#define PRINT_XATTR_RAW		0x02
#define PRINT_XATTR_C		0x04
#define PRINT_XATTR_STATFMT	0x08
#define PRINT_XATTR_NOQUOTES	0x10

extern const char *debug_prog_name;

/* Dump extended attributes */
static void print_xattr_hex(FILE *f, const char *str, int len)
{
	int i;

	for (i = 0; i < len; i++)
		fprintf(f, "%02x ", (unsigned char)str[i]);
}

/* Dump extended attributes */
static void print_xattr_string(FILE *f, const char *str, int len, int flags)
{
	int printable = 0;
	int i;

	if (flags & PRINT_XATTR_RAW) {
		fwrite(str, len, 1, f);
		return;
	}

	if ((flags & PRINT_XATTR_C) == 0) {
		/* check: is string "printable enough?" */
		for (i = 0; i < len; i++)
			if (isprint(str[i]))
				printable++;

		if (printable <= len*7/8)
			flags |= PRINT_XATTR_HEX;
	}

	if (flags & PRINT_XATTR_HEX) {
		print_xattr_hex(f, str, len);
	} else {
		if ((flags & PRINT_XATTR_NOQUOTES) == 0)
			fputc('\"', f);
		print_c_string(f, str, len);
		if ((flags & PRINT_XATTR_NOQUOTES) == 0)
			fputc('\"', f);
	}
}

static void print_xattr(FILE *f, char *name, char *value, size_t value_len,
			int print_flags)
{
	print_xattr_string(f, name, strlen(name), PRINT_XATTR_NOQUOTES);
	fprintf(f, " (%zu)", value_len);
	if ((print_flags & PRINT_XATTR_STATFMT) &&
	    (strcmp(name, "system.data") == 0))
		value_len = 0;
	if (value_len != 0 &&
	    (!(print_flags & PRINT_XATTR_STATFMT) || (value_len < 120))) {
		fprintf(f, " = ");
		print_xattr_string(f, value, value_len, print_flags);
	}
	fputc('\n', f);
}

static int print_acl(FILE *f, char *name, void *value, size_t value_len)
{
	const ext4_acl_header *ext_acl = (const ext4_acl_header *)value;
	const char *cp;

	if (!value ||
	    (value_len < sizeof(ext4_acl_header)) ||
	    (ext_acl->a_version != ext2fs_cpu_to_le32(EXT4_ACL_VERSION)))
		return -EINVAL;

	cp = (const char *)value + sizeof(ext4_acl_header);
	value_len -= sizeof(ext4_acl_header);

	fprintf(f, "%s:\n", name);

	while (value_len > 0) {
		const ext4_acl_entry *disk_entry = (const ext4_acl_entry *)cp;
		posix_acl_xattr_entry entry;
		entry.e_tag = ext2fs_le16_to_cpu(disk_entry->e_tag);
		entry.e_perm = ext2fs_le16_to_cpu(disk_entry->e_perm);

		switch(entry.e_tag) {
			case ACL_USER_OBJ:
			case ACL_USER:
				fprintf(f, "    user:");
				if (entry.e_tag == ACL_USER)
					fprintf(f, "%u",
					ext2fs_le32_to_cpu(disk_entry->e_id));
				break;

			case ACL_GROUP_OBJ:
			case ACL_GROUP:
				fprintf(f, "    group:");
				if (entry.e_tag == ACL_GROUP)
					fprintf(f, "%u",
					ext2fs_le32_to_cpu(disk_entry->e_id));
				break;

			case ACL_MASK:
				fprintf(f, "    mask:");
				break;

			case ACL_OTHER:
				fprintf(f, "    other:");
				break;

			default:
				fprintf(stderr,
					"%s: error: invalid tag %x in ACL\n",
					debug_prog_name, entry.e_tag);
				return -EINVAL;
		}
		fprintf(f, ":");
		fprintf(f, (entry.e_perm & ACL_READ) ? "r" : "-");
		fprintf(f, (entry.e_perm & ACL_WRITE) ? "w" : "-");
		fprintf(f, (entry.e_perm & ACL_EXECUTE) ? "x" : "-");
		fprintf(f, "\n");

		if (entry.e_tag == ACL_USER || entry.e_tag == ACL_GROUP) {
			cp += sizeof(ext4_acl_entry);
			value_len -= sizeof(ext4_acl_entry);
		} else {
			cp += sizeof(ext4_acl_entry_short);
			value_len -= sizeof(ext4_acl_entry_short);
		}
	}

	return 0;
}

static int print_fidstr(FILE *f, char *name, void *value, size_t value_len)
{
	struct filter_fid_old *ff = value;
	int stripe;

	/* Since Lustre 2.4 only the parent FID is stored in filter_fid,
	 * and the self fid is stored in the LMA and is printed below. */
	if (value_len < sizeof(ff->ff_parent)) {
		fprintf(stderr, "%s: error: xattr '%s' too small (%zu bytes)\n",
			debug_prog_name, name, value_len);
		return -EINVAL;
	}
	fid_le_to_cpu(&ff->ff_parent, &ff->ff_parent);
	stripe = fid_ver(&ff->ff_parent); /* stripe index is stored in f_ver */
	ff->ff_parent.f_ver = 0;

	fprintf(f, "fid: ");
	/* Old larger filter_fid should only ever be used with seq = 0.
	 * FID-on-OST should use LMA for FID_SEQ_NORMAL OST objects. */
	if (value_len == sizeof(*ff))
		fprintf(f, "objid=%llu seq=%llu ",
			ext2fs_le64_to_cpu(ff->ff_objid),
			ext2fs_le64_to_cpu(ff->ff_seq));

	fprintf(f, "parent="DFID" stripe=%u", PFID(&ff->ff_parent), stripe);
	if (value_len >= sizeof(struct filter_fid_210)) {
		struct filter_fid_210 *ff_new = value;

		fprintf(f, " stripe_size=%u stripe_count=%u",
			ext2fs_le32_to_cpu(ff_new->ff_stripe_size),
			ext2fs_le32_to_cpu(ff_new->ff_stripe_count));
		if (ff_new->ff_pfl_id != 0)
			fprintf(f, " component_id=%u component_start=%llu "
				"component_end=%llu",
				ext2fs_le32_to_cpu(ff_new->ff_pfl_id),
				ext2fs_le64_to_cpu(ff_new->ff_pfl_start),
				ext2fs_le64_to_cpu(ff_new->ff_pfl_end));
	}

	if (value_len >= sizeof(struct filter_fid)) {
		struct filter_fid *ff_new = value;

		fprintf(f, " layout_version=%u range=%u",
			ext2fs_le32_to_cpu(ff_new->ff_layout_version),
			ext2fs_le32_to_cpu(ff_new->ff_range));
	}

	fprintf(f, "\n");

	return 0;
}

static int print_lmastr(FILE *f, char *name, void *value, size_t value_len)
{
	struct lustre_mdt_attrs *lma = value;
	struct lustre_ost_attrs *loa = value;

	if (value_len < offsetof(typeof(*lma), lma_self_fid) +
			sizeof(lma->lma_self_fid)) {
		fprintf(stderr, "%s: error: xattr '%s' too small (%zu bytes)\n",
			debug_prog_name, name, value_len);
		return -EINVAL;
	}
	fid_le_to_cpu(&lma->lma_self_fid, &lma->lma_self_fid);
	fprintf(f, "lma: fid="DFID" compat=%x incompat=%x\n",
		PFID(&lma->lma_self_fid), ext2fs_le32_to_cpu(lma->lma_compat),
		ext2fs_le32_to_cpu(lma->lma_incompat));
	if (value_len >= offsetof(typeof(*loa), loa_pfl_end) +
		  sizeof(loa->loa_pfl_end)) {
		int idx;
		int cnt;

		fid_le_to_cpu(&loa->loa_parent_fid, &loa->loa_parent_fid);
		idx = loa->loa_parent_fid.f_ver & PFID_STRIPE_COUNT_MASK;
		cnt = loa->loa_parent_fid.f_ver >> PFID_STRIPE_IDX_BITS;
		loa->loa_parent_fid.f_ver = 0;

		fprintf(f, "  fid: parent="DFID" stripe=%u stripe_size=%u "
			"stripe_count=%u", PFID(&loa->loa_parent_fid), idx,
			ext2fs_le32_to_cpu(loa->loa_stripe_size), cnt);
		if (loa->loa_pfl_id != 0)
			fprintf(f, " component_id=%u component_start=%llu "
				"component_end=%llu",
				ext2fs_le32_to_cpu(loa->loa_pfl_id),
				ext2fs_le64_to_cpu(loa->loa_pfl_start),
				ext2fs_le64_to_cpu(loa->loa_pfl_end));
		fprintf(f, "\n");
	}

	return 0;
}

static void print_name(FILE *f, const char *cp, int len)
{
	unsigned char ch;

	while (len--) {
		ch = *cp++;
		if (!isprint(ch) || ch == '\\') {
			if (f)
				fprintf(f, "\\x%02x", ch);
		} else {
			if (f)
				fputc(ch, f);
		}
	}
}

static int print_linkea(FILE *f, char *name, void *value, size_t value_len)
{
	struct link_ea_header *leh = value;
	struct link_ea_entry *lee;
	int i;

	if (value_len < sizeof(*leh) ||
	    value_len < ext2fs_le64_to_cpu(leh->leh_len)) {
		fprintf(stderr, "%s: error: xattr '%s' too small (%zu bytes)\n",
			debug_prog_name, name, value_len);
		return -EINVAL;
	}

	if (ext2fs_le32_to_cpu(leh->leh_magic) != LINK_EA_MAGIC) {
		fprintf(stderr, "%s: error: xattr '%s' bad magic '%#x'\n",
			debug_prog_name, name,
			ext2fs_le32_to_cpu(leh->leh_magic));
		return -EINVAL;
	}

	lee = leh->leh_entry;
	value_len -= sizeof(*leh);

	for (i = 0; i < ext2fs_le32_to_cpu(leh->leh_reccount) &&
		    value_len > 2; i++) {
		int reclen = lee->lee_reclen[0] << 8 | lee->lee_reclen[1];
		struct lu_fid pfid;

		if (value_len < sizeof(*lee) || value_len < reclen) {
			fprintf(stderr,
				"%s: error: xattr '%s' entry %d too small "
				"(%zu bytes)\n",
				debug_prog_name, name, i, value_len);
			return -EINVAL;
		}

		memcpy(&pfid, &lee->lee_parent_fid, sizeof(pfid));
		fid_be_to_cpu(&pfid, &pfid);
		fprintf(f, "%s idx=%u parent="DFID" name='",
			i == 0 ? "linkea:" : "         ", i, PFID(&pfid));
		print_name(f, lee->lee_name, reclen - (int)sizeof(*lee));
		fprintf(f, "'\n");

		lee = (struct link_ea_entry *)((char *)lee + reclen);
		value_len -= reclen;
	}

	return 0;
}

struct dump_attr_pretty {
	const char *dap_name;
	int (*dap_print)(FILE *f, char *name, void *value, size_t value_len);
} dumpers[] = {
	{
		.dap_name = "system.posix_acl_access",
		.dap_print = print_acl,
	},
	{
		.dap_name = "system.posix_acl_default",
		.dap_print = print_acl,
	},
	{
		.dap_name = "trusted.fid",
		.dap_print = print_fidstr,
	},
	{
		.dap_name = "trusted.lma",
		.dap_print = print_lmastr,
	},
	{
		.dap_name = "trusted.link",
		.dap_print = print_linkea,
	},
	{
		.dap_name = NULL,
	}
};

static int dump_attr(char *name, char *value, size_t value_len,
		     ext2_ino_t inode_num, void *data)
{
	struct dump_attr_pretty *dap;
	FILE *out = data;
	int rc = 0;

	fprintf(out, "  ");
	if (EXT2_HAS_INCOMPAT_FEATURE(current_fs->super,
				      EXT4_FEATURE_INCOMPAT_EA_INODE) &&
				      inode_num != 0) {
		fprintf(out, "inode <%u> ", inode_num);
	}

	for (dap = dumpers; dap->dap_name != NULL; dap++) {
		if (strcmp(name, dap->dap_name) == 0) {
			rc = dap->dap_print(out, name, value, value_len);
			break;
		}
	}
	if (dap->dap_name == NULL || rc)
		print_xattr(out, name, value, value_len, PRINT_XATTR_STATFMT);

	return 0;
}

void dump_inode_attributes(FILE *out, ext2_ino_t ino)
{
	struct ext2_xattr_handle *h;
	size_t sz;
	errcode_t err;

	err = ext2fs_xattrs_open(current_fs, ino, &h);
	if (err)
		return;

	err = ext2fs_xattrs_read(h);
	if (err)
		goto out;

	err = ext2fs_xattrs_count(h, &sz);
	if (err || sz == 0)
		goto out;

	fprintf(out, "Extended attributes:\n");
	err = ext2fs_xattrs_iterate(h, dump_attr, out);
	if (err)
		goto out;

out:
	err = ext2fs_xattrs_close(&h);
}

void do_list_xattr(int argc, char **argv, int sci_idx EXT2FS_ATTR((unused)),
		   void *infop EXT2FS_ATTR((unused)))
{
	ext2_ino_t ino;

	if (argc != 2) {
		printf("%s: Usage: %s <file>\n", argv[0],
		       argv[0]);
		return;
	}

	if (check_fs_open(argv[0]))
		return;

	ino = string_to_inode(argv[1]);
	if (!ino)
		return;

	dump_inode_attributes(stdout, ino);
}

void do_get_xattr(int argc, char **argv, int sci_idx EXT2FS_ATTR((unused)),
		  void *infop EXT2FS_ATTR((unused)))
{
	ext2_ino_t ino;
	struct ext2_xattr_handle *h;
	FILE *fp = NULL;
	char *buf = NULL;
	size_t buflen;
	int i;
	int print_flags = 0;
	unsigned int handle_flags = 0;
	errcode_t err;

	reset_getopt();
	while ((i = getopt(argc, argv, "Cf:rxV")) != -1) {
		switch (i) {
		case 'f':
			if (fp)
				fclose(fp);
			fp = fopen(optarg, "w");
			if (fp == NULL) {
				perror(optarg);
				return;
			}
			break;
		case 'r':
			handle_flags |= XATTR_HANDLE_FLAG_RAW;
			break;
		case 'x':
			print_flags |= PRINT_XATTR_HEX;
			break;
		case 'V':
			print_flags |= PRINT_XATTR_RAW|
				PRINT_XATTR_NOQUOTES;
			break;
		case 'C':
			print_flags |= PRINT_XATTR_C;
			break;
		default:
			goto usage;
		}
	}

	if (optind != argc - 2) {
	usage:
		printf("%s: Usage: %s [-f outfile]|[-xVC] [-r] <file> <attr>\n",
			       argv[0], argv[0]);

		goto out2;
	}

	if (check_fs_open(argv[0]))
		goto out2;

	ino = string_to_inode(argv[optind]);
	if (!ino)
		goto out2;

	err = ext2fs_xattrs_open(current_fs, ino, &h);
	if (err)
		goto out2;

	err = ext2fs_xattrs_flags(h, &handle_flags, NULL);
	if (err)
		goto out;

	err = ext2fs_xattrs_read(h);
	if (err)
		goto out;

	err = ext2fs_xattr_get(h, argv[optind + 1], (void **)&buf, &buflen);
	if (err)
		goto out;

	if (fp) {
		fwrite(buf, buflen, 1, fp);
	} else {
		if (print_flags & PRINT_XATTR_RAW) {
			if (print_flags & (PRINT_XATTR_HEX|PRINT_XATTR_C))
				print_flags &= ~PRINT_XATTR_RAW;
			print_xattr_string(stdout, buf, buflen, print_flags);
		} else {
			print_xattr(stdout, argv[optind + 1],
				    buf, buflen, print_flags);
		}
		printf("\n");
	}

	ext2fs_free_mem(&buf);
out:
	ext2fs_xattrs_close(&h);
	if (err)
		com_err(argv[0], err, "while getting extended attribute");
out2:
	if (fp)
		fclose(fp);
}

void do_set_xattr(int argc, char **argv, int sci_idx EXT2FS_ATTR((unused)),
		  void *infop EXT2FS_ATTR((unused)))
{
	ext2_ino_t ino;
	struct ext2_xattr_handle *h;
	FILE *fp = NULL;
	char *buf = NULL;
	size_t buflen;
	unsigned int handle_flags = 0;
	int i;
	errcode_t err;

	reset_getopt();
	while ((i = getopt(argc, argv, "f:r")) != -1) {
		switch (i) {
		case 'f':
			if (fp)
				fclose(fp);
			fp = fopen(optarg, "r");
			if (fp == NULL) {
				perror(optarg);
				return;
			}
			break;
		case 'r':
			handle_flags |= XATTR_HANDLE_FLAG_RAW;
			break;
		default:
			goto print_usage;
		}
	}

	if (!(fp && optind == argc - 2) && !(!fp && optind == argc - 3)) {
	print_usage:
		printf("Usage:\t%s [-r] <file> <attr> <value>\n", argv[0]);
		printf("\t%s -f <value_file> [-r] <file> <attr>\n", argv[0]);
		goto out2;
	}

	if (check_fs_open(argv[0]))
		goto out2;
	if (check_fs_read_write(argv[0]))
		goto out2;
	if (check_fs_bitmaps(argv[0]))
		goto out2;

	ino = string_to_inode(argv[optind]);
	if (!ino)
		goto out2;

	err = ext2fs_xattrs_open(current_fs, ino, &h);
	if (err)
		goto out2;

	err = ext2fs_xattrs_flags(h, &handle_flags, NULL);
	if (err)
		goto out;

	err = ext2fs_xattrs_read(h);
	if (err)
		goto out;

	if (fp) {
		err = ext2fs_get_mem(current_fs->blocksize, &buf);
		if (err)
			goto out;
		buflen = fread(buf, 1, current_fs->blocksize, fp);
	} else {
		buf = argv[optind + 2];
		buflen = parse_c_string(buf);
	}

	err = ext2fs_xattr_set(h, argv[optind + 1], buf, buflen);
out:
	ext2fs_xattrs_close(&h);
	if (err)
		com_err(argv[0], err, "while setting extended attribute");
out2:
	if (fp) {
		fclose(fp);
		ext2fs_free_mem(&buf);
	}
}

void do_rm_xattr(int argc, char **argv, int sci_idx EXT2FS_ATTR((unused)),
		 void *infop EXT2FS_ATTR((unused)))
{
	ext2_ino_t ino;
	struct ext2_xattr_handle *h;
	int i;
	errcode_t err;

	if (argc < 3) {
		printf("%s: Usage: %s <file> <attrs>...\n", argv[0], argv[0]);
		return;
	}

	if (check_fs_open(argv[0]))
		return;
	if (check_fs_read_write(argv[0]))
		return;
	if (check_fs_bitmaps(argv[0]))
		return;

	ino = string_to_inode(argv[1]);
	if (!ino)
		return;

	err = ext2fs_xattrs_open(current_fs, ino, &h);
	if (err)
		return;

	err = ext2fs_xattrs_read(h);
	if (err)
		goto out;

	for (i = 2; i < argc; i++) {
		err = ext2fs_xattr_remove(h, argv[i]);
		if (err)
			goto out;
	}
out:
	ext2fs_xattrs_close(&h);
	if (err)
		com_err(argv[0], err, "while removing extended attribute");
}

/*
 * Return non-zero if the string has a minimal number of non-printable
 * characters.
 */
static int is_mostly_printable(const char *cp, int len)
{
	int	np = 0;

	if (len < 0)
		len = strlen(cp);

	while (len--) {
		if (!isprint(*cp++)) {
			np++;
			if (np > 3)
				return 0;
		}
	}
	return 1;
}

static void safe_print(FILE *f, const char *cp, int len)
{
	unsigned char	ch;

	if (len < 0)
		len = strlen(cp);

	while (len--) {
		ch = *cp++;
		if (ch > 128) {
			fputs("M-", f);
			ch -= 128;
		}
		if ((ch < 32) || (ch == 0x7f)) {
			fputc('^', f);
			ch ^= 0x40; /* ^@, ^A, ^B; ^? for DEL */
		}
		fputc(ch, f);
	}
}

static void dump_xattr_raw_entries(FILE *f, unsigned char *buf,
				   unsigned int start, unsigned int len,
				   unsigned value_start)
{
	struct ext2_ext_attr_entry ent;
	unsigned int off = start;
	unsigned int vstart;

	while (off < len) {
		if ((*(__u16 *) (buf + off)) == 0) {
			fprintf(f, "last entry found at offset %u (%04o)\n",
				off, off);
			break;
		}
		if ((off + sizeof(struct ext2_ext_attr_entry)) >= len) {
			fprintf(f, "xattr buffer overrun at %u (len = %u)\n",
				off, len);
			break;
		}
#if WORDS_BIGENDIAN
		ext2fs_swap_ext_attr_entry(&ent,
			(struct ext2_ext_attr_entry *) (buf + off));
#else
		ent = *((struct ext2_ext_attr_entry *) (buf + off));
#endif
		fprintf(f, "offset = %d (%04o), name_len = %u, "
			"name_index = %u\n",
			off, off, ent.e_name_len, ent.e_name_index);
		vstart = value_start + ent.e_value_offs;
		fprintf(f, "value_offset = %d (%04o), value_inum = %u, "
			"value_size = %u\n", ent.e_value_offs,
			vstart, ent.e_value_inum, ent.e_value_size);
		off += sizeof(struct ext2_ext_attr_entry);
		fprintf(f, "name = ");
		if ((off + ent.e_name_len) >= len)
			fprintf(f, "<runs off end>");
		else
			safe_print(f, (char *)(buf + off), ent.e_name_len);
		fputc('\n', f);
		if (ent.e_value_size == 0)
			goto skip_value;
		fprintf(f, "value = ");
		if (ent.e_value_inum)
			fprintf(f, "<ino %u>", ent.e_value_inum);
		else if (ent.e_value_offs >= len ||
			 (vstart + ent.e_value_size) > len)
			fprintf(f, "<runs off end>");
		else if (is_mostly_printable((char *)(buf + vstart),
					ent.e_value_size))
			safe_print(f, (char *)(buf + vstart),
				   ent.e_value_size);
		else {
			fprintf(f, "<hexdump>\n");
			do_byte_hexdump(f, (unsigned char *)(buf + vstart),
					ent.e_value_size);
		}
		fputc('\n', f);
	skip_value:
		fputc('\n', f);
		off += (ent.e_name_len + 3) & ~3;
	}
}

void raw_inode_xattr_dump(FILE *f, unsigned char *buf, unsigned int len)
{
	__u32 magic = ext2fs_le32_to_cpu(*((__le32 *) buf));

	fprintf(f, "magic = %08x, length = %u, value_start =4 \n\n",
		magic, len);
	if (magic == EXT2_EXT_ATTR_MAGIC)
		dump_xattr_raw_entries(f, buf, 4, len, 4);
}

void block_xattr_dump(FILE *f, unsigned char *buf, unsigned int len)
{
	struct ext2_ext_attr_header header;

#ifdef WORDS_BIGENDIAN
	ext2fs_swap_ext_attr_header(&header,
				    (struct ext2_ext_attr_header *) buf);
#else
	header = *((struct ext2_ext_attr_header *) buf);
#endif
	fprintf(f, "magic = %08x, length = %u\n", header.h_magic, len);
	if (header.h_magic != EXT2_EXT_ATTR_MAGIC)
		return;
	fprintf(f, "refcount = %u, blocks = %u\n", header.h_refcount,
		header.h_blocks);
	fprintf(f, "hash = %08x, checksum = %08x\n", header.h_hash,
		header.h_checksum);
	fprintf(f, "reserved: %08x %08x %08x\n\n", header.h_reserved[0],
		header.h_reserved[1], header.h_reserved[2]);

	dump_xattr_raw_entries(f, buf,
			       sizeof(struct ext2_ext_attr_header), len, 0);
}
