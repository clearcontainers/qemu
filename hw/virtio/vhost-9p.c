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

#include <sys/ioctl.h>
#include "qemu/osdep.h"
#include "standard-headers/linux/virtio_9p.h"
#include "qapi/error.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"
#include "migration/migration.h"
#include "qemu/error-report.h"
#include "hw/virtio/vhost-9p.h"
#include "qemu/iov.h"
#include "monitor/monitor.h"

#define VHOST_SET_PATH 3

enum {
    VHOST_9P_SAVEVM_VERSION = 0,
    VHOST_9P_MAX_REQ = 128,
};


static void vhost_9p_get_config(VirtIODevice *vdev, uint8_t *out)
{
    VHost9p *p9dev = VHOST_9P(vdev);
    struct virtio_9p_config *cfg;
    VHost9pConf *c = &p9dev->conf;

    size_t len = strlen(c->tag);
    cfg = g_malloc0(sizeof(struct virtio_9p_config) + len);
    virtio_stw_p(vdev, &cfg->tag_len, len);
    /* We don't copy the terminating null to config space */
    memcpy(cfg->tag, c->tag, len);
    memcpy(out, cfg, p9dev->config_size);
    g_free(cfg);
}

static void vhost_9p_start(VirtIODevice *vdev)
{
    VHost9p *p9dev = VHOST_9P(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;
    int i;

    if (!k->set_guest_notifiers) {
        error_report("binding does not support guest notifiers");
        return;
    }

    ret = vhost_dev_enable_notifiers(&p9dev->vhost_dev, vdev);
    if (ret < 0) {
        error_report("Error enabling host notifiers: %d", -ret);
        return;
    }

    ret = k->set_guest_notifiers(qbus->parent, p9dev->vhost_dev.nvqs, true);
    if (ret < 0) {
        error_report("Error binding guest notifier: %d", -ret);
        goto err_host_notifiers;
    }

    p9dev->vhost_dev.acked_features = vdev->guest_features;
    ret = vhost_dev_start(&p9dev->vhost_dev, vdev);
    if (ret < 0) {
        error_report("Error starting vhost: %d", -ret);
        goto err_guest_notifiers;
    }

    /* guest_notifier_mask/pending not used yet, so just unmask
     * everything here.  virtio-pci will do the right thing by
     * enabling/disabling irqfd.
     */
    for (i = 0; i < p9dev->vhost_dev.nvqs; i++) {
        vhost_virtqueue_mask(&p9dev->vhost_dev, vdev, i, false);
    }

    return;

  err_guest_notifiers:
    k->set_guest_notifiers(qbus->parent, p9dev->vhost_dev.nvqs, false);
  err_host_notifiers:
    vhost_dev_disable_notifiers(&p9dev->vhost_dev, vdev);
}

static void vhost_9p_stop(VirtIODevice *vdev)
{
    VHost9p *p9dev = VHOST_9P(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;

    if (!k->set_guest_notifiers) {
        return;
    }

    vhost_dev_stop(&p9dev->vhost_dev, vdev);

    ret = k->set_guest_notifiers(qbus->parent, p9dev->vhost_dev.nvqs, false);
    if (ret < 0) {
        error_report("vhost guest notifier cleanup failed: %d", ret);
        return;
    }

    vhost_dev_disable_notifiers(&p9dev->vhost_dev, vdev);
}

static void vhost_9p_set_status(VirtIODevice *vdev, uint8_t status)
{
    VHost9p *p9dev = VHOST_9P(vdev);
    bool should_start = status & VIRTIO_CONFIG_S_DRIVER_OK;

    if (!vdev->vm_running) {
        should_start = false;
    }

    if (p9dev->vhost_dev.started == should_start) {
        return;
    }

    if (should_start) {
        vhost_9p_start(vdev);
    } else {
        vhost_9p_stop(vdev);
    }
}

static uint64_t vhost_9p_get_features(VirtIODevice *vdev,
                                         uint64_t requested_features,
                                         Error **errp)
{
    virtio_add_feature(&requested_features, VIRTIO_9P_MOUNT_TAG);
    return requested_features;
}

static void vhost_9p_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    /* Do nothing */
}

static void vhost_9p_guest_notifier_mask(VirtIODevice *vdev, int idx,
                                            bool mask)
{
    VHost9p *p9dev = VHOST_9P(vdev);

    vhost_virtqueue_mask(&p9dev->vhost_dev, vdev, idx, mask);
}

static bool vhost_9p_guest_notifier_pending(VirtIODevice *vdev, int idx)
{
    VHost9p *p9dev = VHOST_9P(vdev);

    return vhost_virtqueue_pending(&p9dev->vhost_dev, idx);
}

static int vhost_9p_save(QEMUFile *f, void *opaque, size_t size,
                         VMStateField *field, QJSON *vmdesc)
{
    VHost9p *p9dev = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(p9dev);

    /* At this point, backend must be stopped, otherwise
     * it might keep writing to memory. */
    assert(!p9dev->vhost_dev.started);
    virtio_save(vdev, f);
    return 0;
}

static int vhost_9p_load(QEMUFile *f, void *opaque, size_t size,
                         VMStateField *field)
{
    VHost9p *p9dev = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(p9dev);
    int ret;

    ret = virtio_load(vdev, f, VHOST_9P_SAVEVM_VERSION);
    if (ret) {
        return ret;
    }

    return 0;
}

static void vhost_9p_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHost9p *p9dev = VHOST_9P(dev);
    VHost9pConf *c = &p9dev->conf;
    int vhostfd;
    int ret;

    if (p9dev->conf.vhostfd) {
        vhostfd = monitor_fd_param(cur_mon, p9dev->conf.vhostfd, errp);
        if (vhostfd == -1) {
            error_prepend(errp, "vhost-9p: unable to parse vhostfd: ");
            return;
        }
    } else {
        vhostfd = open("/dev/vhost-9p", O_RDWR);
        if (vhostfd < 0) {
            error_setg_errno(errp, -errno,
                             "vhost-9p: failed to open vhost device");
            return;
        }
    }

    p9dev->config_size = sizeof(struct virtio_9p_config) + strlen(c->tag);
    virtio_init(vdev, "vhost-9p", VIRTIO_ID_9P, p9dev->config_size);

    virtio_add_queue(vdev, VHOST_9P_MAX_REQ, vhost_9p_handle_output);

    p9dev->vhost_dev.nvqs = ARRAY_SIZE(p9dev->vhost_vqs);
    p9dev->vhost_dev.vqs = p9dev->vhost_vqs;
    ret = vhost_dev_init(&p9dev->vhost_dev, (void *)(uintptr_t)vhostfd,
                         VHOST_BACKEND_TYPE_KERNEL, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "vhost-9p: vhost_dev_init failed");
        goto err_virtio;
    }
    ioctl(vhostfd, VHOST_SET_PATH, c->path);

    return;

  err_virtio:
    virtio_cleanup(vdev);
    close(vhostfd);
    return;
}

static void vhost_9p_device_unrealize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHost9p *p9dev = VHOST_9P(dev);

    /* This will stop vhost backend if appropriate. */
    vhost_9p_set_status(vdev, 0);

    vhost_dev_cleanup(&p9dev->vhost_dev);
    virtio_cleanup(vdev);
}

static Property vhost_9p_properties[] = {
    DEFINE_PROP_STRING("vhostfd", VHost9p, conf.vhostfd),
    DEFINE_PROP_STRING("mount_tag", VHost9p, conf.tag),
    DEFINE_PROP_STRING("path", VHost9p, conf.path),
    DEFINE_PROP_END_OF_LIST(),
};

const VMStateInfo  virtio_vmstate_9p_info = {
    .name = "vhost_9p",
    .get = vhost_9p_load,
    .put = vhost_9p_save,
};

static const VMStateDescription vmstate_virtio_vhost_9p = {
    .name = "vhost_9p",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]) {
        {
            .name = "vhost_9p",
            .info = &virtio_vmstate_9p_info,
            .flags = VHOST_9P_SAVEVM_VERSION,
        },
        VMSTATE_END_OF_LIST()
    },
};

static void vhost_9p_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->props = vhost_9p_properties;
    dc->vmsd = &vmstate_virtio_vhost_9p;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    vdc->realize = vhost_9p_device_realize;
    vdc->unrealize = vhost_9p_device_unrealize;
    vdc->get_features = vhost_9p_get_features;
    vdc->get_config = vhost_9p_get_config;
    vdc->set_status = vhost_9p_set_status;
    vdc->guest_notifier_mask = vhost_9p_guest_notifier_mask;
    vdc->guest_notifier_pending = vhost_9p_guest_notifier_pending;
}

static const TypeInfo vhost_9p_info = {
    .name = TYPE_VHOST_9P,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VHost9p),
    .class_init = vhost_9p_class_init,
};

static void vhost_9p_register_types(void)
{
    type_register_static(&vhost_9p_info);
}

type_init(vhost_9p_register_types)
