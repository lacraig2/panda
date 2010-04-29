/*
 * Virtio 9p backend
 *
 * Copyright IBM, Corp. 2010
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "virtio.h"
#include "pc.h"
#include "qemu_socket.h"
#include "virtio-9p.h"
#include "fsdev/qemu-fsdev.h"
#include "virtio-9p-debug.h"

int dotu = 1;
int debug_9p_pdu;

static int v9fs_do_lstat(V9fsState *s, V9fsString *path, struct stat *stbuf)
{
    return s->ops->lstat(&s->ctx, path->data, stbuf);
}

static int v9fs_do_setuid(V9fsState *s, uid_t uid)
{
    return s->ops->setuid(&s->ctx, uid);
}

static ssize_t v9fs_do_readlink(V9fsState *s, V9fsString *path, V9fsString *buf)
{
    ssize_t len;

    buf->data = qemu_malloc(1024);

    len = s->ops->readlink(&s->ctx, path->data, buf->data, 1024 - 1);
    if (len > -1) {
        buf->size = len;
        buf->data[len] = 0;
    }

    return len;
}

static int v9fs_do_close(V9fsState *s, int fd)
{
    return s->ops->close(&s->ctx, fd);
}

static int v9fs_do_closedir(V9fsState *s, DIR *dir)
{
    return s->ops->closedir(&s->ctx, dir);
}

static void v9fs_string_init(V9fsString *str)
{
    str->data = NULL;
    str->size = 0;
}

static void v9fs_string_free(V9fsString *str)
{
    qemu_free(str->data);
    str->data = NULL;
    str->size = 0;
}

static void v9fs_string_null(V9fsString *str)
{
    v9fs_string_free(str);
}

static int number_to_string(void *arg, char type)
{
    unsigned int ret = 0;

    switch (type) {
    case 'u': {
        unsigned int num = *(unsigned int *)arg;

        do {
            ret++;
            num = num/10;
        } while (num);
        break;
    }
    default:
        printf("Number_to_string: Unknown number format\n");
        return -1;
    }

    return ret;
}

static int v9fs_string_alloc_printf(char **strp, const char *fmt, va_list ap)
{
    va_list ap2;
    char *iter = (char *)fmt;
    int len = 0;
    int nr_args = 0;
    char *arg_char_ptr;
    unsigned int arg_uint;

    /* Find the number of %'s that denotes an argument */
    for (iter = strstr(iter, "%"); iter; iter = strstr(iter, "%")) {
        nr_args++;
        iter++;
    }

    len = strlen(fmt) - 2*nr_args;

    if (!nr_args) {
        goto alloc_print;
    }

    va_copy(ap2, ap);

    iter = (char *)fmt;

    /* Now parse the format string */
    for (iter = strstr(iter, "%"); iter; iter = strstr(iter, "%")) {
        iter++;
        switch (*iter) {
        case 'u':
            arg_uint = va_arg(ap2, unsigned int);
            len += number_to_string((void *)&arg_uint, 'u');
            break;
        case 's':
            arg_char_ptr = va_arg(ap2, char *);
            len += strlen(arg_char_ptr);
            break;
        case 'c':
            len += 1;
            break;
        default:
            fprintf(stderr,
		    "v9fs_string_alloc_printf:Incorrect format %c", *iter);
            return -1;
        }
        iter++;
    }

alloc_print:
    *strp = qemu_malloc((len + 1) * sizeof(**strp));

    return vsprintf(*strp, fmt, ap);
}

static void v9fs_string_sprintf(V9fsString *str, const char *fmt, ...)
{
    va_list ap;
    int err;

    v9fs_string_free(str);

    va_start(ap, fmt);
    err = v9fs_string_alloc_printf(&str->data, fmt, ap);
    BUG_ON(err == -1);
    va_end(ap);

    str->size = err;
}

static void v9fs_string_copy(V9fsString *lhs, V9fsString *rhs)
{
    v9fs_string_free(lhs);
    v9fs_string_sprintf(lhs, "%s", rhs->data);
}

static size_t v9fs_string_size(V9fsString *str)
{
    return str->size;
}

static V9fsFidState *lookup_fid(V9fsState *s, int32_t fid)
{
    V9fsFidState *f;

    for (f = s->fid_list; f; f = f->next) {
        if (f->fid == fid) {
            v9fs_do_setuid(s, f->uid);
            return f;
        }
    }

    return NULL;
}

static V9fsFidState *alloc_fid(V9fsState *s, int32_t fid)
{
    V9fsFidState *f;

    f = lookup_fid(s, fid);
    if (f) {
        return NULL;
    }

    f = qemu_mallocz(sizeof(V9fsFidState));

    f->fid = fid;
    f->fd = -1;
    f->dir = NULL;

    f->next = s->fid_list;
    s->fid_list = f;

    return f;
}

static int free_fid(V9fsState *s, int32_t fid)
{
    V9fsFidState **fidpp, *fidp;

    for (fidpp = &s->fid_list; *fidpp; fidpp = &(*fidpp)->next) {
        if ((*fidpp)->fid == fid) {
            break;
        }
    }

    if (*fidpp == NULL) {
        return -ENOENT;
    }

    fidp = *fidpp;
    *fidpp = fidp->next;

    if (fidp->fd != -1) {
        v9fs_do_close(s, fidp->fd);
    }
    if (fidp->dir) {
        v9fs_do_closedir(s, fidp->dir);
    }
    v9fs_string_free(&fidp->path);
    qemu_free(fidp);

    return 0;
}

#define P9_QID_TYPE_DIR         0x80
#define P9_QID_TYPE_SYMLINK     0x02

#define P9_STAT_MODE_DIR        0x80000000
#define P9_STAT_MODE_APPEND     0x40000000
#define P9_STAT_MODE_EXCL       0x20000000
#define P9_STAT_MODE_MOUNT      0x10000000
#define P9_STAT_MODE_AUTH       0x08000000
#define P9_STAT_MODE_TMP        0x04000000
#define P9_STAT_MODE_SYMLINK    0x02000000
#define P9_STAT_MODE_LINK       0x01000000
#define P9_STAT_MODE_DEVICE     0x00800000
#define P9_STAT_MODE_NAMED_PIPE 0x00200000
#define P9_STAT_MODE_SOCKET     0x00100000
#define P9_STAT_MODE_SETUID     0x00080000
#define P9_STAT_MODE_SETGID     0x00040000
#define P9_STAT_MODE_SETVTX     0x00010000

#define P9_STAT_MODE_TYPE_BITS (P9_STAT_MODE_DIR |          \
                                P9_STAT_MODE_SYMLINK |      \
                                P9_STAT_MODE_LINK |         \
                                P9_STAT_MODE_DEVICE |       \
                                P9_STAT_MODE_NAMED_PIPE |   \
                                P9_STAT_MODE_SOCKET)

/* This is the algorithm from ufs in spfs */
static void stat_to_qid(const struct stat *stbuf, V9fsQID *qidp)
{
    size_t size;

    size = MIN(sizeof(stbuf->st_ino), sizeof(qidp->path));
    memcpy(&qidp->path, &stbuf->st_ino, size);
    qidp->version = stbuf->st_mtime ^ (stbuf->st_size << 8);
    qidp->type = 0;
    if (S_ISDIR(stbuf->st_mode)) {
        qidp->type |= P9_QID_TYPE_DIR;
    }
    if (S_ISLNK(stbuf->st_mode)) {
        qidp->type |= P9_QID_TYPE_SYMLINK;
    }
}

static int fid_to_qid(V9fsState *s, V9fsFidState *fidp, V9fsQID *qidp)
{
    struct stat stbuf;
    int err;

    err = v9fs_do_lstat(s, &fidp->path, &stbuf);
    if (err) {
        return err;
    }

    stat_to_qid(&stbuf, qidp);
    return 0;
}

static V9fsPDU *alloc_pdu(V9fsState *s)
{
    V9fsPDU *pdu = NULL;

    if (!QLIST_EMPTY(&s->free_list)) {
	pdu = QLIST_FIRST(&s->free_list);
	QLIST_REMOVE(pdu, next);
    }
    return pdu;
}

static void free_pdu(V9fsState *s, V9fsPDU *pdu)
{
    if (pdu) {
	QLIST_INSERT_HEAD(&s->free_list, pdu, next);
    }
}

size_t pdu_packunpack(void *addr, struct iovec *sg, int sg_count,
                        size_t offset, size_t size, int pack)
{
    int i = 0;
    size_t copied = 0;

    for (i = 0; size && i < sg_count; i++) {
        size_t len;
        if (offset >= sg[i].iov_len) {
            /* skip this sg */
            offset -= sg[i].iov_len;
            continue;
        } else {
            len = MIN(sg[i].iov_len - offset, size);
            if (pack) {
                memcpy(sg[i].iov_base + offset, addr, len);
            } else {
                memcpy(addr, sg[i].iov_base + offset, len);
            }
            size -= len;
            copied += len;
            addr += len;
            if (size) {
                offset = 0;
                continue;
            }
        }
    }

    return copied;
}

static size_t pdu_unpack(void *dst, V9fsPDU *pdu, size_t offset, size_t size)
{
    return pdu_packunpack(dst, pdu->elem.out_sg, pdu->elem.out_num,
                         offset, size, 0);
}

static size_t pdu_pack(V9fsPDU *pdu, size_t offset, const void *src,
                        size_t size)
{
    return pdu_packunpack((void *)src, pdu->elem.in_sg, pdu->elem.in_num,
                             offset, size, 1);
}

static int pdu_copy_sg(V9fsPDU *pdu, size_t offset, int rx, struct iovec *sg)
{
    size_t pos = 0;
    int i, j;
    struct iovec *src_sg;
    unsigned int num;

    if (rx) {
        src_sg = pdu->elem.in_sg;
        num = pdu->elem.in_num;
    } else {
        src_sg = pdu->elem.out_sg;
        num = pdu->elem.out_num;
    }

    j = 0;
    for (i = 0; i < num; i++) {
        if (offset <= pos) {
            sg[j].iov_base = src_sg[i].iov_base;
            sg[j].iov_len = src_sg[i].iov_len;
            j++;
        } else if (offset < (src_sg[i].iov_len + pos)) {
            sg[j].iov_base = src_sg[i].iov_base;
            sg[j].iov_len = src_sg[i].iov_len;
            sg[j].iov_base += (offset - pos);
            sg[j].iov_len -= (offset - pos);
            j++;
        }
        pos += src_sg[i].iov_len;
    }

    return j;
}

static size_t pdu_unmarshal(V9fsPDU *pdu, size_t offset, const char *fmt, ...)
{
    size_t old_offset = offset;
    va_list ap;
    int i;

    va_start(ap, fmt);
    for (i = 0; fmt[i]; i++) {
        switch (fmt[i]) {
        case 'b': {
            uint8_t *valp = va_arg(ap, uint8_t *);
            offset += pdu_unpack(valp, pdu, offset, sizeof(*valp));
            break;
        }
        case 'w': {
            uint16_t val, *valp;
            valp = va_arg(ap, uint16_t *);
            val = le16_to_cpupu(valp);
            offset += pdu_unpack(&val, pdu, offset, sizeof(val));
            *valp = val;
            break;
        }
        case 'd': {
            uint32_t val, *valp;
            valp = va_arg(ap, uint32_t *);
            val = le32_to_cpupu(valp);
            offset += pdu_unpack(&val, pdu, offset, sizeof(val));
            *valp = val;
            break;
        }
        case 'q': {
            uint64_t val, *valp;
            valp = va_arg(ap, uint64_t *);
            val = le64_to_cpup(valp);
            offset += pdu_unpack(&val, pdu, offset, sizeof(val));
            *valp = val;
            break;
        }
        case 'v': {
            struct iovec *iov = va_arg(ap, struct iovec *);
            int *iovcnt = va_arg(ap, int *);
            *iovcnt = pdu_copy_sg(pdu, offset, 0, iov);
            break;
        }
        case 's': {
            V9fsString *str = va_arg(ap, V9fsString *);
            offset += pdu_unmarshal(pdu, offset, "w", &str->size);
            /* FIXME: sanity check str->size */
            str->data = qemu_malloc(str->size + 1);
            offset += pdu_unpack(str->data, pdu, offset, str->size);
            str->data[str->size] = 0;
            break;
        }
        case 'Q': {
            V9fsQID *qidp = va_arg(ap, V9fsQID *);
            offset += pdu_unmarshal(pdu, offset, "bdq",
                        &qidp->type, &qidp->version, &qidp->path);
            break;
        }
        case 'S': {
            V9fsStat *statp = va_arg(ap, V9fsStat *);
            offset += pdu_unmarshal(pdu, offset, "wwdQdddqsssssddd",
                        &statp->size, &statp->type, &statp->dev,
                        &statp->qid, &statp->mode, &statp->atime,
                        &statp->mtime, &statp->length,
                        &statp->name, &statp->uid, &statp->gid,
                        &statp->muid, &statp->extension,
                        &statp->n_uid, &statp->n_gid,
                        &statp->n_muid);
            break;
        }
        default:
            break;
        }
    }

    va_end(ap);

    return offset - old_offset;
}

static size_t pdu_marshal(V9fsPDU *pdu, size_t offset, const char *fmt, ...)
{
    size_t old_offset = offset;
    va_list ap;
    int i;

    va_start(ap, fmt);
    for (i = 0; fmt[i]; i++) {
        switch (fmt[i]) {
        case 'b': {
            uint8_t val = va_arg(ap, int);
            offset += pdu_pack(pdu, offset, &val, sizeof(val));
            break;
        }
        case 'w': {
            uint16_t val;
            cpu_to_le16w(&val, va_arg(ap, int));
            offset += pdu_pack(pdu, offset, &val, sizeof(val));
            break;
        }
        case 'd': {
            uint32_t val;
            cpu_to_le32w(&val, va_arg(ap, uint32_t));
            offset += pdu_pack(pdu, offset, &val, sizeof(val));
            break;
        }
        case 'q': {
            uint64_t val;
            cpu_to_le64w(&val, va_arg(ap, uint64_t));
            offset += pdu_pack(pdu, offset, &val, sizeof(val));
            break;
        }
        case 'v': {
            struct iovec *iov = va_arg(ap, struct iovec *);
            int *iovcnt = va_arg(ap, int *);
            *iovcnt = pdu_copy_sg(pdu, offset, 1, iov);
            break;
        }
        case 's': {
            V9fsString *str = va_arg(ap, V9fsString *);
            offset += pdu_marshal(pdu, offset, "w", str->size);
            offset += pdu_pack(pdu, offset, str->data, str->size);
            break;
        }
        case 'Q': {
            V9fsQID *qidp = va_arg(ap, V9fsQID *);
            offset += pdu_marshal(pdu, offset, "bdq",
                        qidp->type, qidp->version, qidp->path);
            break;
        }
        case 'S': {
            V9fsStat *statp = va_arg(ap, V9fsStat *);
            offset += pdu_marshal(pdu, offset, "wwdQdddqsssssddd",
                        statp->size, statp->type, statp->dev,
                        &statp->qid, statp->mode, statp->atime,
                        statp->mtime, statp->length, &statp->name,
                        &statp->uid, &statp->gid, &statp->muid,
                        &statp->extension, statp->n_uid,
                        statp->n_gid, statp->n_muid);
            break;
        }
        default:
            break;
        }
    }
    va_end(ap);

    return offset - old_offset;
}

static void complete_pdu(V9fsState *s, V9fsPDU *pdu, ssize_t len)
{
    int8_t id = pdu->id + 1; /* Response */

    if (len < 0) {
        V9fsString str;
        int err = -len;

        str.data = strerror(err);
        str.size = strlen(str.data);

        len = 7;
        len += pdu_marshal(pdu, len, "s", &str);
        if (dotu) {
            len += pdu_marshal(pdu, len, "d", err);
        }

        id = P9_RERROR;
    }

    /* fill out the header */
    pdu_marshal(pdu, 0, "dbw", (int32_t)len, id, pdu->tag);

    /* keep these in sync */
    pdu->size = len;
    pdu->id = id;

    /* push onto queue and notify */
    virtqueue_push(s->vq, &pdu->elem, len);

    /* FIXME: we should batch these completions */
    virtio_notify(&s->vdev, s->vq);

    free_pdu(s, pdu);
}

static mode_t v9mode_to_mode(uint32_t mode, V9fsString *extension)
{
    mode_t ret;

    ret = mode & 0777;
    if (mode & P9_STAT_MODE_DIR) {
        ret |= S_IFDIR;
    }

    if (dotu) {
        if (mode & P9_STAT_MODE_SYMLINK) {
            ret |= S_IFLNK;
        }
        if (mode & P9_STAT_MODE_SOCKET) {
            ret |= S_IFSOCK;
        }
        if (mode & P9_STAT_MODE_NAMED_PIPE) {
            ret |= S_IFIFO;
        }
        if (mode & P9_STAT_MODE_DEVICE) {
            if (extension && extension->data[0] == 'c') {
                ret |= S_IFCHR;
            } else {
                ret |= S_IFBLK;
            }
        }
    }

    if (!(ret&~0777)) {
        ret |= S_IFREG;
    }

    if (mode & P9_STAT_MODE_SETUID) {
        ret |= S_ISUID;
    }
    if (mode & P9_STAT_MODE_SETGID) {
        ret |= S_ISGID;
    }
    if (mode & P9_STAT_MODE_SETVTX) {
        ret |= S_ISVTX;
    }

    return ret;
}

static int donttouch_stat(V9fsStat *stat)
{
    if (stat->type == -1 &&
        stat->dev == -1 &&
        stat->qid.type == -1 &&
        stat->qid.version == -1 &&
        stat->qid.path == -1 &&
        stat->mode == -1 &&
        stat->atime == -1 &&
        stat->mtime == -1 &&
        stat->length == -1 &&
        !stat->name.size &&
        !stat->uid.size &&
        !stat->gid.size &&
        !stat->muid.size &&
        stat->n_uid == -1 &&
        stat->n_gid == -1 &&
        stat->n_muid == -1) {
        return 1;
    }

    return 0;
}

static void v9fs_stat_free(V9fsStat *stat)
{
    v9fs_string_free(&stat->name);
    v9fs_string_free(&stat->uid);
    v9fs_string_free(&stat->gid);
    v9fs_string_free(&stat->muid);
    v9fs_string_free(&stat->extension);
}

static uint32_t stat_to_v9mode(const struct stat *stbuf)
{
    uint32_t mode;

    mode = stbuf->st_mode & 0777;
    if (S_ISDIR(stbuf->st_mode)) {
        mode |= P9_STAT_MODE_DIR;
    }

    if (dotu) {
        if (S_ISLNK(stbuf->st_mode)) {
            mode |= P9_STAT_MODE_SYMLINK;
        }

        if (S_ISSOCK(stbuf->st_mode)) {
            mode |= P9_STAT_MODE_SOCKET;
        }

        if (S_ISFIFO(stbuf->st_mode)) {
            mode |= P9_STAT_MODE_NAMED_PIPE;
        }

        if (S_ISBLK(stbuf->st_mode) || S_ISCHR(stbuf->st_mode)) {
            mode |= P9_STAT_MODE_DEVICE;
        }

        if (stbuf->st_mode & S_ISUID) {
            mode |= P9_STAT_MODE_SETUID;
        }

        if (stbuf->st_mode & S_ISGID) {
            mode |= P9_STAT_MODE_SETGID;
        }

        if (stbuf->st_mode & S_ISVTX) {
            mode |= P9_STAT_MODE_SETVTX;
        }
    }

    return mode;
}

static int stat_to_v9stat(V9fsState *s, V9fsString *name,
                            const struct stat *stbuf,
                            V9fsStat *v9stat)
{
    int err;
    const char *str;

    memset(v9stat, 0, sizeof(*v9stat));

    stat_to_qid(stbuf, &v9stat->qid);
    v9stat->mode = stat_to_v9mode(stbuf);
    v9stat->atime = stbuf->st_atime;
    v9stat->mtime = stbuf->st_mtime;
    v9stat->length = stbuf->st_size;

    v9fs_string_null(&v9stat->uid);
    v9fs_string_null(&v9stat->gid);
    v9fs_string_null(&v9stat->muid);

    if (dotu) {
        v9stat->n_uid = stbuf->st_uid;
        v9stat->n_gid = stbuf->st_gid;
        v9stat->n_muid = 0;

        v9fs_string_null(&v9stat->extension);

        if (v9stat->mode & P9_STAT_MODE_SYMLINK) {
            err = v9fs_do_readlink(s, name, &v9stat->extension);
            if (err == -1) {
                err = -errno;
                return err;
            }
            v9stat->extension.data[err] = 0;
            v9stat->extension.size = err;
        } else if (v9stat->mode & P9_STAT_MODE_DEVICE) {
            v9fs_string_sprintf(&v9stat->extension, "%c %u %u",
                    S_ISCHR(stbuf->st_mode) ? 'c' : 'b',
                    major(stbuf->st_rdev), minor(stbuf->st_rdev));
        } else if (S_ISDIR(stbuf->st_mode) || S_ISREG(stbuf->st_mode)) {
            v9fs_string_sprintf(&v9stat->extension, "%s %u",
                    "HARDLINKCOUNT", stbuf->st_nlink);
        }
    }

    str = strrchr(name->data, '/');
    if (str) {
        str += 1;
    } else {
        str = name->data;
    }

    v9fs_string_sprintf(&v9stat->name, "%s", str);

    v9stat->size = 61 +
        v9fs_string_size(&v9stat->name) +
        v9fs_string_size(&v9stat->uid) +
        v9fs_string_size(&v9stat->gid) +
        v9fs_string_size(&v9stat->muid) +
        v9fs_string_size(&v9stat->extension);
    return 0;
}

static struct iovec *adjust_sg(struct iovec *sg, int len, int *iovcnt)
{
    while (len && *iovcnt) {
        if (len < sg->iov_len) {
            sg->iov_len -= len;
            sg->iov_base += len;
            len = 0;
        } else {
            len -= sg->iov_len;
            sg++;
            *iovcnt -= 1;
        }
    }

    return sg;
}

static struct iovec *cap_sg(struct iovec *sg, int cap, int *cnt)
{
    int i;
    int total = 0;

    for (i = 0; i < *cnt; i++) {
        if ((total + sg[i].iov_len) > cap) {
            sg[i].iov_len -= ((total + sg[i].iov_len) - cap);
            i++;
            break;
        }
        total += sg[i].iov_len;
    }

    *cnt = i;

    return sg;
}

static void print_sg(struct iovec *sg, int cnt)
{
    int i;

    printf("sg[%d]: {", cnt);
    for (i = 0; i < cnt; i++) {
        if (i) {
            printf(", ");
        }
        printf("(%p, %zd)", sg[i].iov_base, sg[i].iov_len);
    }
    printf("}\n");
}

static void v9fs_dummy(V9fsState *s, V9fsPDU *pdu)
{
    /* Note: The following have been added to prevent GCC from complaining
     * They will be removed in the subsequent patches */
    (void)pdu_unmarshal;
    (void) complete_pdu;
    (void) v9fs_string_init;
    (void) v9fs_string_free;
    (void) v9fs_string_null;
    (void) v9fs_string_sprintf;
    (void) v9fs_string_copy;
    (void) v9fs_string_size;
    (void) v9fs_do_lstat;
    (void) v9fs_do_setuid;
    (void) v9fs_do_readlink;
    (void) v9fs_do_close;
    (void) v9fs_do_closedir;
    (void) alloc_fid;
    (void) free_fid;
    (void) fid_to_qid;
    (void) v9mode_to_mode;
    (void) donttouch_stat;
    (void) v9fs_stat_free;
    (void) stat_to_v9stat;
    (void) adjust_sg;
    (void) cap_sg;
    (void) print_sg;
}

static void v9fs_version(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_attach(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_stat(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_walk(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_clunk(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_open(V9fsState *s, V9fsPDU *pdu)
{    if (debug_9p_pdu) {
        pprint_pdu(pdu);
     }
}

static void v9fs_read(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_write(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_create(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_flush(V9fsState *s, V9fsPDU *pdu)
{
    v9fs_dummy(s, pdu);
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_remove(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

static void v9fs_wstat(V9fsState *s, V9fsPDU *pdu)
{
    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }
}

typedef void (pdu_handler_t)(V9fsState *s, V9fsPDU *pdu);

static pdu_handler_t *pdu_handlers[] = {
    [P9_TVERSION] = v9fs_version,
    [P9_TATTACH] = v9fs_attach,
    [P9_TSTAT] = v9fs_stat,
    [P9_TWALK] = v9fs_walk,
    [P9_TCLUNK] = v9fs_clunk,
    [P9_TOPEN] = v9fs_open,
    [P9_TREAD] = v9fs_read,
#if 0
    [P9_TAUTH] = v9fs_auth,
#endif
    [P9_TFLUSH] = v9fs_flush,
    [P9_TCREATE] = v9fs_create,
    [P9_TWRITE] = v9fs_write,
    [P9_TWSTAT] = v9fs_wstat,
    [P9_TREMOVE] = v9fs_remove,
};

static void submit_pdu(V9fsState *s, V9fsPDU *pdu)
{
    pdu_handler_t *handler;

    if (debug_9p_pdu) {
        pprint_pdu(pdu);
    }

    BUG_ON(pdu->id >= ARRAY_SIZE(pdu_handlers));

    handler = pdu_handlers[pdu->id];
    BUG_ON(handler == NULL);

    handler(s, pdu);
}

static void handle_9p_output(VirtIODevice *vdev, VirtQueue *vq)
{
    V9fsState *s = (V9fsState *)vdev;
    V9fsPDU *pdu;
    ssize_t len;

    while ((pdu = alloc_pdu(s)) &&
            (len = virtqueue_pop(vq, &pdu->elem)) != 0) {
        uint8_t *ptr;

        BUG_ON(pdu->elem.out_num == 0 || pdu->elem.in_num == 0);
        BUG_ON(pdu->elem.out_sg[0].iov_len < 7);

        ptr = pdu->elem.out_sg[0].iov_base;

        memcpy(&pdu->size, ptr, 4);
        pdu->id = ptr[4];
        memcpy(&pdu->tag, ptr + 5, 2);

        submit_pdu(s, pdu);
    }

    free_pdu(s, pdu);
}

static uint32_t virtio_9p_get_features(VirtIODevice *vdev, uint32_t features)
{
    features |= 1 << VIRTIO_9P_MOUNT_TAG;
    return features;
}

static V9fsState *to_virtio_9p(VirtIODevice *vdev)
{
    return (V9fsState *)vdev;
}

static void virtio_9p_get_config(VirtIODevice *vdev, uint8_t *config)
{
    struct virtio_9p_config *cfg;
    V9fsState *s = to_virtio_9p(vdev);

    cfg = qemu_mallocz(sizeof(struct virtio_9p_config) +
                        s->tag_len);
    stw_raw(&cfg->tag_len, s->tag_len);
    memcpy(cfg->tag, s->tag, s->tag_len);
    memcpy(config, cfg, s->config_size);
    qemu_free(cfg);
}

VirtIODevice *virtio_9p_init(DeviceState *dev, V9fsConf *conf)
 {
    V9fsState *s;
    int i, len;
    struct stat stat;
    FsTypeEntry *fse;


    s = (V9fsState *)virtio_common_init("virtio-9p",
                                    VIRTIO_ID_9P,
                                    sizeof(struct virtio_9p_config)+
                                    MAX_TAG_LEN,
                                    sizeof(V9fsState));

    /* initialize pdu allocator */
    QLIST_INIT(&s->free_list);
    for (i = 0; i < (MAX_REQ - 1); i++) {
	QLIST_INSERT_HEAD(&s->free_list, &s->pdus[i], next);
    }

    s->vq = virtio_add_queue(&s->vdev, MAX_REQ, handle_9p_output);

    fse = get_fsdev_fsentry(conf->fsdev_id);

    if (!fse) {
        /* We don't have a fsdev identified by fsdev_id */
        fprintf(stderr, "Virtio-9p device couldn't find fsdev "
                    "with the id %s\n", conf->fsdev_id);
        exit(1);
    }

    if (!fse->path || !conf->tag) {
        /* we haven't specified a mount_tag or the path */
        fprintf(stderr, "fsdev with id %s needs path "
                "and Virtio-9p device needs mount_tag arguments\n",
                conf->fsdev_id);
        exit(1);
    }

    if (lstat(fse->path, &stat)) {
        fprintf(stderr, "share path %s does not exist\n", fse->path);
        exit(1);
    } else if (!S_ISDIR(stat.st_mode)) {
        fprintf(stderr, "share path %s is not a directory \n", fse->path);
        exit(1);
    }

    s->ctx.fs_root = qemu_strdup(fse->path);
    len = strlen(conf->tag);
    if (len > MAX_TAG_LEN) {
        len = MAX_TAG_LEN;
    }
    /* s->tag is non-NULL terminated string */
    s->tag = qemu_malloc(len);
    memcpy(s->tag, conf->tag, len);
    s->tag_len = len;
    s->ctx.uid = -1;

    s->ops = fse->ops;
    s->vdev.get_features = virtio_9p_get_features;
    s->config_size = sizeof(struct virtio_9p_config) +
                        s->tag_len;
    s->vdev.get_config = virtio_9p_get_config;

    return &s->vdev;
}
