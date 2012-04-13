#ifndef LFSCK_H
#define LFSCK_H

/* This is unfortunately needed for older lustre_user.h to be usable */
#define LASSERT(cond)		do { } while (0)

#ifdef HAVE_LUSTRE_LUSTREAPI_H
#include <lustre/lustreapi.h>
#elif HAVE_LUSTRE_LIBLUSTREAPI_H
#include <lustre/liblustreapi.h>
#endif

#ifndef DFID
#define DFID "[%#llx:0x%x:0x%x]"
#define PFID(fid) (unsigned long long)fid_seq(fid), fid_oid(fid), fid_ver(fid)
struct lu_fid {
	__u64   f_seq;
	__u32   f_oid;
	__u32   f_ver;
};
#endif /* !DFID */

/* Unfortunately, neither the 1.8 or 2.x lustre_idl.h file is suitable
 * for inclusion by userspace programs because of external dependencies.
 * Define the minimum set of replacement functions here until that is fixed. */
#ifndef HAVE_LUSTRE_LUSTRE_IDL_H
#define fid_seq(fid) ((fid)->f_seq)
#define fid_oid(fid) ((fid)->f_oid)
#define fid_ver(fid) ((fid)->f_ver)

static inline void fid_be_to_cpu(struct lu_fid *dst, struct lu_fid *src)
{
	dst->f_seq = ext2fs_be64_to_cpu(src->f_seq);
	dst->f_oid = ext2fs_be32_to_cpu(src->f_oid);
	dst->f_ver = ext2fs_be32_to_cpu(src->f_ver);
}
#endif

#endif /* LFSCK_H */
