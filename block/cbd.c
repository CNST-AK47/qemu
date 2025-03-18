/*
 *     Copyright (c) 2020 NetEase Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Project: Curve
 *
 * History:
 *          2018/10/10  Wenyu Zhou   Initial version
 *
 * QEMU Block driver for Curve Block Device
 *
 */

#include "qemu/osdep.h"
#include "libcbd.h"
#include "qapi/error.h"
#include "qemu/uri.h"
#include "block/block_int.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/qobject-output-visitor.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qstring.h"
#include "qemu/cutils.h"

#define SUPPORT_AIO

#define CBD_MAX_CONF_NAME_SIZE 128
#define CBD_MAX_CONF_VAL_SIZE 512
#define CBD_MAX_CONF_SIZE 1024
#define CBD_MAX_POOL_NAME_SIZE 128
#define CBD_MAX_NAME_SIZE 128

typedef struct CBDDiskInfo {
    char*   filename;
    int64_t size;
} CBDDiskInfo;

typedef struct BDRVCBDState {
    CBDDiskInfo info;
    CurveFd     fd;
} BDRVCBDState;

typedef struct CbdAioContext {
    BlockAIOCB      bac;
    CurveAioContext cac;
    void*           iov;
} CbdAioContext;


static int qemu_cbd_next_tok(char *dst, int dst_len,
                             char *src, char delim,
                             const char *name,
                             char **p, Error **errp)
{
    int l;
    char *end;

    *p = NULL;

    if (delim != '\0') {
        for (end = src; *end; ++end) {
            if (*end == delim) {
                break;
            }
            if (*end == '\\' && end[1] != '\0') {
                end++;
            }
        }
        if (*end == delim) {
            *p = end + 1;
            *end = '\0';
        }
    }
    l = strlen(src);
    if (l >= dst_len) {
        error_setg(errp, "%s too long", name);
        return -EINVAL;
    } else if (l == 0) {
        error_setg(errp, "%s too short", name);
        return -EINVAL;
    }

    pstrcpy(dst, dst_len, src);

    return 0;
}

static void qemu_cbd_unescape(char *src)
{
    char *p;

    for (p = src; *src; ++src, ++p) {
        if (*src == '\\' && src[1] != '\0') {
            src++;
        }
        *p = *src;
    }
    *p = '\0';
}


static QemuOptsList cbd_runtime_opts = {
    .name = "cbd",
    .head = QTAILQ_HEAD_INITIALIZER(cbd_runtime_opts.head),
    .desc = {
        {
            .name = "pool",
            .type = QEMU_OPT_STRING,
            .help = "Curve pool name",
        },
        {
            .name = "file",
            .type = QEMU_OPT_STRING,
            .help = "Curve file in the pool",
        },
        {
            .name = "conf",
            .type = QEMU_OPT_STRING,
            .help = "Curve config file location",
        },
        { /* end of list */ }
    },
};

static int cbd_parse_filename(const char *filename,
                              char *pool, int pool_len,
                              char *snap, int snap_len,
                              char *name, int name_len,
                              char *conf, int conf_len,
                              Error **errp)
{
    const char *start;
    char *p, *buf;
    int ret;

    if (!strstart(filename, "cbd:", &start)) {
        error_setg(errp, "File name must start with 'cbd:'");
        return -EINVAL;
    }

    buf = g_strdup(start);
    p = buf;
    *snap = '\0';
    *conf = '\0';

    ret = qemu_cbd_next_tok(pool, pool_len, p,
                            '/', "pool name", &p, errp);
    if (ret < 0 || !p) {
        ret = -EINVAL;
        goto done;
    }
    qemu_cbd_unescape(pool);

    if (strchr(p, '@')) {
        ret = qemu_cbd_next_tok(name, name_len, p,
                                '@', "file name", &p, errp);
    } else {
        ret = qemu_cbd_next_tok(name, name_len, p,
                                ':', "file name", &p, errp);
    }
    qemu_cbd_unescape(name);
    if (ret < 0 || !p) {
        ret = -EINVAL;
        goto done;
    }

    ret = qemu_cbd_next_tok(conf, conf_len, p,
                            '\0', "configuration", &p, errp);

done:
    g_free(buf);
    return ret;
}

static QemuOptsList runtime_opts = {
    .name = "cbd",
    .head = QTAILQ_HEAD_INITIALIZER(runtime_opts.head),
    .desc = {
        {
            .name = "filename",
            .type = QEMU_OPT_STRING,
            .help = "Specification of the cbd image",
        },
        { /* end of list */ }
    },
};

static int cbd_open(BlockDriverState *bs, QDict *options, int flags,
                    Error **errp)
{
    BDRVCBDState *s = bs->opaque;
    CurveOptions opt;
    int ret = -EINVAL;

    char pool[CBD_MAX_POOL_NAME_SIZE];
    char snap_buf[CBD_MAX_NAME_SIZE];
    char conf[CBD_MAX_CONF_SIZE];
    char name[CBD_MAX_NAME_SIZE];
    QemuOpts *opts;
    Error *local_err = NULL;
    const char *filename;

    opts = qemu_opts_create(&runtime_opts, NULL, 0, &error_abort);
    qemu_opts_absorb_qdict(opts, options, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        goto failed_opts;
    }

    filename = qemu_opt_get(opts, "filename");

    if (cbd_parse_filename(filename, pool, sizeof(pool),
                           snap_buf, sizeof(snap_buf),
                           name, sizeof(name),
                           conf, sizeof(conf), errp) < 0) {
        ret = -EINVAL;
        goto failed_opts;
    }

    s->info.filename = g_strdup(name);
    opt.conf = g_strdup(conf);
    qdict_del(options, "conf");

    qdict_del(options, "pool");

#ifdef CBD_BACKEND_EXT4
    if ((opt.datahome = g_strdup(qdict_get_try_str(options, "datahome")))) {
        qdict_del(options, "datahome");
    } else {
        opt.datahome = (char *)"./";
    }
#endif

    ret = cbd_lib_init(&opt);
    if (ret) {
        goto failed_opts;
    }

    s->fd = cbd_lib_open(s->info.filename);
    if (s->fd < 0) {
        ret = -1;
        goto failed_opts;
    }
    s->info.size = cbd_lib_filesize(s->info.filename);
    qemu_opts_del(opts);
    return 0;

failed_opts:
    qemu_opts_del(opts);
    return ret;
}

static void cbd_close(BlockDriverState *bs)
{
    BDRVCBDState *s = bs->opaque;

    cbd_lib_close(s->fd);
}

#ifndef SUPPORT_AIO

static int cbd_preadv(BlockDriverState *bs, uint64_t offset,
                      uint64_t bytes, QEMUIOVector *qiov, int flags)
{
    int ret;
    char* buf = NULL;
    BDRVCBDState *s = bs->opaque;

    if (!bytes) {
        ret = -1;
        goto cbd_preadv_cleanup;
    }

    buf = (char *)g_malloc(bytes);
    if (!buf) {
        ret = -1;
        goto cbd_preadv_cleanup;
    }

    ret = cbd_lib_pread(s->fd, buf, offset, bytes);

    if (ret) {
        goto cbd_preadv_cleanup;
    } else {
        qemu_iovec_from_buf(qiov, 0, buf, bytes);
    }

cbd_preadv_cleanup:
    if (buf) {
        g_free(buf);
    }

    return ret;
}

static int cbd_pwritev(BlockDriverState *bs, uint64_t offset,
                       uint64_t bytes, QEMUIOVector *qiov, int flags)
{
    int ret;
    char* buf = NULL;
    BDRVCBDState *s = bs->opaque;

    if (!bytes) {
        ret = -1;
        goto cbd_pwritev_cleanup;
    }

    buf = (char *)g_malloc(bytes);
    if (!buf) {
        ret = -1;
        goto cbd_pwritev_cleanup;
    }

    qemu_iovec_to_buf(qiov, 0, buf, bytes);
    ret = cbd_lib_pwrite(s->fd, buf, offset, bytes);

cbd_pwritev_cleanup:
    if (buf) {
        g_free(buf);
    }

    return ret;
}

#else

static const AIOCBInfo cbd_aiocb_info = {
    .aiocb_size = sizeof(CbdAioContext),
};

static void cbd_complete_aio(void *cbd_opaque)
{
    CbdAioContext* cbdac = (CbdAioContext *)cbd_opaque;
    CurveAioContext* cac = &cbdac->cac;
    int ret = cac->ret;
    BlockAIOCB* bac = &cbdac->bac;
    QEMUIOVector* qiov = (QEMUIOVector *)cbdac->iov;
    BlockCompletionFunc* cb = bac->cb;
    char* buf = cac->buf;
    void* opaque = bac->opaque;

    if (cac->op == LIBCURVE_OP_READ) {
        qemu_iovec_from_buf(qiov, 0, buf, cac->length);
    }

    cb(opaque, ret);

    qemu_aio_unref(cbdac);
    g_free(buf);
}
static inline CbdAioContext* cbd_aio_context_entry(CurveAioContext* cac)
{
    uint64_t offset = (uint64_t)(&((CbdAioContext *)0)->cac);
    return (CbdAioContext *)((char *)cac - offset);
}

static void cbd_aio_callback(struct CurveAioContext* cac)
{
    CbdAioContext* cbdac = cbd_aio_context_entry(cac);
    BlockAIOCB* bac = &cbdac->bac;

    aio_bh_schedule_oneshot(bdrv_get_aio_context(bac->bs),
                            cbd_complete_aio, cbdac);
}

static BlockAIOCB *cbd_aio_start(BlockDriverState *bs,
                                 uint64_t offset, uint64_t bytes,
                                 QEMUIOVector *qiov,
                                 BlockCompletionFunc *cb,
                                 void *opaque,
                                 LIBCURVE_OP op)
{
    int ret;
    char* buf = NULL;
    CbdAioContext *cbdac = NULL;
    CurveAioContext *cac = NULL;
    BlockAIOCB *bac = NULL;
    BDRVCBDState *s = bs->opaque;

    // assert(bytes <= CBD_MAX_BUF_LEN);

    cbdac = (CbdAioContext *)qemu_aio_get(&cbd_aiocb_info, bs, cb, opaque);
    bac = &cbdac->bac;
    cac = &cbdac->cac;
    if (!bytes) {
        goto cbd_aio_start_cleanup;
    }

    buf = (char *)g_malloc(bytes);
    if (!buf) {
        goto cbd_aio_start_cleanup;
    }


    memset(cac, 0, sizeof(CurveAioContext));
    cac->offset = offset;
    cac->length = bytes;
    cac->op = op;
    cac->cb = cbd_aio_callback;
    cac->buf = buf;

    cbdac->iov = qiov;

    if (op == LIBCURVE_OP_READ) {
        ret = cbd_lib_aio_pread(s->fd, &cbdac->cac);
    } else if (op == LIBCURVE_OP_WRITE) {
        memset(buf, 0, bytes);
        qemu_iovec_to_buf(qiov, 0, buf, bytes);

        ret = cbd_lib_aio_pwrite(s->fd, &cbdac->cac);
    } else {
        ret = -1;
    }

    if (ret) {
        goto cbd_aio_start_cleanup;
    }
    return bac;

cbd_aio_start_cleanup:
    qemu_aio_unref(cbdac);
    if (buf) {
        g_free(buf);
    }

    return NULL;
}

static BlockAIOCB *cbd_aio_preadv(BlockDriverState *bs,
                                      int64_t sector_num,
                                      QEMUIOVector *qiov,
                                      int nb_sectors,
                                      BlockCompletionFunc *cb,
                                      void *opaque)
  {
    BlockAIOCB* bac = NULL;

    bac = cbd_aio_start(bs, sector_num << BDRV_SECTOR_BITS,
      (int64_t) nb_sectors << BDRV_SECTOR_BITS,
      qiov, cb, opaque, LIBCURVE_OP_READ);

    return bac;
}

static BlockAIOCB *cbd_aio_pwritev(BlockDriverState *bs,
                                       int64_t sector_num,
                                       QEMUIOVector *qiov,
                                       int nb_sectors,
                                       BlockCompletionFunc *cb,
                                       void *opaque)
{
    BlockAIOCB* bac = NULL;

    bac = cbd_aio_start(bs, sector_num << BDRV_SECTOR_BITS,
      (int64_t) nb_sectors << BDRV_SECTOR_BITS,
      qiov, cb, opaque, LIBCURVE_OP_WRITE);

    return bac;

}

#endif

static int cbd_co_flush(BlockDriverState *bs)
{
    BDRVCBDState *s = bs->opaque;
    int ret;

    ret = cbd_lib_sync(s->fd);

    return ret;
}

static void cbd_refresh_limits(BlockDriverState *bs, Error **errp)
{
    bs->bl.request_alignment = 4096;
}

static int64_t cbd_getlength(BlockDriverState *bs)
{
    BDRVCBDState *s = bs->opaque;

    return cbd_lib_filesize(s->info.filename);
}

static int cbd_truncate(BlockDriverState *bs, int64_t offset)
{
    BDRVCBDState *s = bs->opaque;
    int r;

    r = cbd_lib_resize(s->info.filename, offset);
    if (r < 0) {
        return r;
    }

    return 0;
}

static BlockDriver bdrv_cbd = {
    .format_name                = "cbd",
    .protocol_name              = "cbd",
    .instance_size              = sizeof(BDRVCBDState),
    .bdrv_file_open             = cbd_open,
    .bdrv_close                 = cbd_close,
#ifndef SUPPORT_AIO
    .bdrv_co_preadv             = cbd_preadv,
    .bdrv_co_pwritev            = cbd_pwritev,
#else
    .bdrv_aio_readv             = cbd_aio_preadv,
    .bdrv_aio_writev            = cbd_aio_pwritev,
#endif
    .bdrv_co_flush_to_disk      = cbd_co_flush,
    .bdrv_refresh_limits        = cbd_refresh_limits,
    .bdrv_getlength             = cbd_getlength,
    .bdrv_truncate              = cbd_truncate,
};

static void bdrv_cbd_init(void)
{
    bdrv_register(&bdrv_cbd);
}

block_init(bdrv_cbd_init);

