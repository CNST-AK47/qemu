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
 */

#ifndef __LIBCBD_H__
#define __LIBCBD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <aio.h>

// #define CBD_BACKEND_FAKE

#ifndef CBD_BACKEND_FAKE
#define CBD_BACKEND_LIBCURVE
#else
#define CBD_BACKEND_EXT4
#endif

#define CBD_MAX_FILE_PATH_LEN   1024
#define CBD_MAX_BUF_LEN         1024 * 1024 * 32

typedef int CurveFd;

typedef enum LIBCURVE_ERROR {
    LIBCURVE_ERROR_NOERROR,
    LIBCURVE_ERROR_UNKNOWN,
    LIBCURVE_ERROR_MAX,
} LIBCURVE_ERROR;

typedef enum LIBCURVE_OP {
    LIBCURVE_OP_READ,
    LIBCURVE_OP_WRITE,
    LIBCURVE_OP_MAX,
} LIBCURVE_OP;

struct CurveAioContext;

typedef void (*LibCurveAioCallBack)(struct CurveAioContext* context);

typedef struct CurveAioContext {
    off_t               offset;
    size_t              length;
    int                 ret;
    LIBCURVE_OP         op;
    LibCurveAioCallBack cb;
    void*               buf;
} CurveAioContext;

typedef struct CurveOptions {
    bool    inited;
    char*   conf;
#ifdef CBD_BACKEND_EXT4
    char*   datahome;
#endif
} CurveOptions;

int cbd_ext4_init(const CurveOptions* options);
int cbd_ext4_fini(void);
int cbd_ext4_open(const char* filename);
int cbd_ext4_close(int fd);
int cbd_ext4_pread(int fd, void* buf, off_t offset, size_t length);
int cbd_ext4_pwrite(int fd, const void* buf, off_t offset, size_t length);
int cbd_ext4_aio_pread(int fd, CurveAioContext* context);
int cbd_ext4_aio_pwrite(int fd, CurveAioContext* context);
int cbd_ext4_sync(int fd);
int64_t cbd_ext4_filesize(const char* filename);

int cbd_libcurve_init(const CurveOptions* options);
int cbd_libcurve_fini(void);
int cbd_libcurve_open(const char* filename);
int cbd_libcurve_close(int fd);
int cbd_libcurve_pread(int fd, void* buf, off_t offset, size_t length);
int cbd_libcurve_pwrite(int fd, const void* buf, off_t offset, size_t length);
int cbd_libcurve_aio_pread(int fd, CurveAioContext* context);
int cbd_libcurve_aio_pwrite(int fd, CurveAioContext* context);
int cbd_libcurve_sync(int fd);
int64_t cbd_libcurve_filesize(const char* filename);
int cbd_libcurve_resize(const char* filename, int64_t size);

#ifndef CBD_BACKEND_FAKE
#define cbd_lib_init        cbd_libcurve_init
#define cbd_lib_fini        cbd_libcurve_fini
#define cbd_lib_open        cbd_libcurve_open
#define cbd_lib_close       cbd_libcurve_close
#define cbd_lib_pread       cbd_libcurve_pread
#define cbd_lib_pwrite      cbd_libcurve_pwrite
#define cbd_lib_aio_pread   cbd_libcurve_aio_pread
#define cbd_lib_aio_pwrite  cbd_libcurve_aio_pwrite
#define cbd_lib_sync        cbd_libcurve_sync
#define cbd_lib_filesize    cbd_libcurve_filesize
#define cbd_lib_resize      cbd_libcurve_resize
#else
#define cbd_lib_init        cbd_ext4_init
#define cbd_lib_fini        cbd_ext4_fini
#define cbd_lib_open        cbd_ext4_open
#define cbd_lib_close       cbd_ext4_close
#define cbd_lib_pread       cbd_ext4_pread
#define cbd_lib_pwrite      cbd_ext4_pwrite
#define cbd_lib_aio_pread   cbd_ext4_aio_pread
#define cbd_lib_aio_pwrite  cbd_ext4_aio_pwrite
#define cbd_lib_sync        cbd_ext4_sync
#define cbd_lib_filesize    cbd_ext4_filesize
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // __LIBCBD_H__

