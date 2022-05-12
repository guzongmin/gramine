/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2022 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

#include <asm/fcntl.h>

#include "api.h"
#include "pal.h"
#include "perm.h"
#include "shim_fs.h"
#include "shim_lock.h"
#include "shim_socket.h"
#include "stat.h"

static int close(struct shim_handle* handle) {
    assert(handle->type == TYPE_SOCK);
    if (lock_created(&handle->info.sock.lock)) {
        destroy_lock(&handle->info.sock.lock);
    }
    /* No need for atomics - we are releaseing the last reference, nothing can access it anymore. */
    if (handle->info.sock.pal_handle) {
        DkObjectClose(handle->info.sock.pal_handle);
    }
    return 0;
}

static ssize_t read(struct shim_handle* handle, void* buf, size_t size, file_off_t* pos) {
    __UNUSED(pos);
    struct iovec iov = {
        .iov_base = buf,
        .iov_len = size,
    };
    return do_recvmsg(handle, &iov, /*iov_len=*/1, /*addr=*/NULL, /*addrlen=*/NULL, /*flags=*/0);
}

static ssize_t write(struct shim_handle* handle, const void* buf, size_t size, file_off_t* pos) {
    __UNUSED(pos);
    struct iovec iov = {
        .iov_base = (void*)buf,
        .iov_len = size,
    };
    return do_sendmsg(handle, &iov, /*iov_len=*/1, /*addr=*/NULL, /*addrlen=*/0, /*flags=*/0);
}

static ssize_t readv(struct shim_handle* handle, struct iovec* iov, size_t iov_len,
                     file_off_t* pos) {
    __UNUSED(pos);
    return do_recvmsg(handle, iov, iov_len, /*addr=*/NULL, /*addrlen=*/NULL, /*flags=*/0);
}

static ssize_t writev(struct shim_handle* handle, struct iovec* iov, size_t iov_len,
                      file_off_t* pos) {
    __UNUSED(pos);
    return do_sendmsg(handle, iov, iov_len, /*addr=*/NULL, /*addrlen=*/0, /*flags=*/0);
}

static int hstat(struct shim_handle* handle, struct stat* stat) {
    __UNUSED(handle);
    assert(stat);

    memset(stat, 0, sizeof(*stat));

    /* TODO: what do we put in `dev` and `ino`? */
    stat->st_dev = 0;
    stat->st_ino = 0;
    stat->st_mode = S_IFSOCK | PERM_rwxrwxrwx;
    stat->st_nlink = 1;
    stat->st_blksize = PAGE_SIZE;

    /* TODO: maybe set `st_size` - query PAL for pending size? */

    return 0;
}

static int setflags(struct shim_handle* handle, int flags) {
    assert(handle->type == TYPE_SOCK);

    if (!WITHIN_MASK(flags, O_NONBLOCK)) {
        return -EINVAL;
    }

    bool nonblocking = flags & O_NONBLOCK;

    PAL_HANDLE pal_handle = __atomic_load_n(&handle->info.sock.pal_handle, __ATOMIC_ACQUIRE);
    if (!pal_handle) {
        log_warning("Trying to set flags on not bound / not connected UNIX socket. This is not "
                    "supported in Gramine.");
        return -EINVAL;
    }

    PAL_STREAM_ATTR attr;
    int ret = DkStreamAttributesQueryByHandle(pal_handle, &attr);
    if (ret < 0) {
        return pal_to_unix_errno(ret);
    }

    if (attr.nonblocking != nonblocking) {
        attr.nonblocking = nonblocking;
        ret = DkStreamAttributesSetByHandle(pal_handle, &attr);
        ret = pal_to_unix_errno(ret);
    } else {
        ret = 0;
    }

    return ret;
}

static int checkout(struct shim_handle* handle) {
    assert(handle->type == TYPE_SOCK);
    struct shim_sock_handle* sock = &handle->info.sock;
    sock->ops = NULL;
    clear_lock(&sock->lock);
    return 0;
}

static int checkin(struct shim_handle* handle) {
    assert(handle->type == TYPE_SOCK);
    struct shim_sock_handle* sock = &handle->info.sock;
    switch (sock->domain) {
        case AF_UNIX:
            sock->ops = &sock_unix_ops;
            break;
        case AF_INET:
        case AF_INET6:
            sock->ops = &sock_ip_ops;
            break;
        default:
            BUG();
    }
    if (!create_lock(&sock->lock)) {
        return -ENOMEM;
    }
    return 0;
}

struct shim_fs_ops socket_fs_ops = {
    .close    = close,
    .read     = read,
    .write    = write,
    .readv    = readv,
    .writev   = writev,
    .hstat    = hstat,
    .setflags = setflags,
    .checkout = checkout,
    .checkin  = checkin,
};

struct shim_fs socket_builtin_fs = {
    .name   = "socket",
    .fs_ops = &socket_fs_ops,
};
