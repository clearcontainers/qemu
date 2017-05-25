/*
 * vhost-9p device
 *
 * Copyright 2016 Intel Corporation
 *
 * Authors:
 *  Yuankai Guo <yuankai.guo@intel.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#ifndef _QEMU_VHOST_9P_H
#define _QEMU_VHOST_9P_H

#include "hw/virtio/virtio.h"
#include "hw/virtio/vhost.h"

#define TYPE_VHOST_9P "vhost-9p-device"
#define VHOST_9P(obj) \
        OBJECT_CHECK(VHost9p, (obj), TYPE_VHOST_9P)

typedef struct {
    char *vhostfd;
} VHost9pConf;

typedef struct {
    /*< private >*/
    VirtIODevice parent;
    VHost9pConf conf;
    struct vhost_virtqueue vhost_vqs[1];
    struct vhost_dev vhost_dev;
    size_t config_size;

    /*< public >*/
} VHost9p;

#endif /* _QEMU_VHOST_9P_H */
