/*
 * Copyright (C) 2009      Citrix Ltd.
 * Author Vincent Hanquez <vincent.hanquez@eu.citrix.com>
 * Author Stefano Stabellini <stefano.stabellini@eu.citrix.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h> /* for write, unlink and close */
#include <stdint.h>
#include <inttypes.h>
#include <dirent.h>
#include <assert.h>

#include "libxl.h"
#include "libxl_utils.h"
#include "libxl_internal.h"
#include "flexarray.h"

static int libxl_create_pci_backend(libxl_ctx *ctx, uint32_t domid, libxl_device_pci *pcidev, int num)
{
    flexarray_t *front;
    flexarray_t *back;
    unsigned int boffset = 0;
    unsigned int foffset = 0;
    libxl_device device;
    int i;

    front = flexarray_make(16, 1);
    if (!front)
        return ERROR_NOMEM;
    back = flexarray_make(16, 1);
    if (!back)
        return ERROR_NOMEM;

    XL_LOG(ctx, XL_LOG_DEBUG, "Creating pci backend");

    /* add pci device */
    device.backend_devid = 0;
    device.backend_domid = 0;
    device.backend_kind = DEVICE_PCI;
    device.devid = 0;
    device.domid = domid;
    device.kind = DEVICE_PCI;

    flexarray_set(back, boffset++, "frontend-id");
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "%d", domid));
    flexarray_set(back, boffset++, "online");
    flexarray_set(back, boffset++, "1");
    flexarray_set(back, boffset++, "state");
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "%d", 1));
    flexarray_set(back, boffset++, "domain");
    flexarray_set(back, boffset++, libxl_domid_to_name(ctx, domid));
    for (i = 0; i < num; i++) {
        flexarray_set(back, boffset++, libxl_sprintf(ctx, "key-%d", i));
        flexarray_set(back, boffset++, libxl_sprintf(ctx, PCI_BDF, pcidev->domain, pcidev->bus, pcidev->dev, pcidev->func));
        flexarray_set(back, boffset++, libxl_sprintf(ctx, "dev-%d", i));
        flexarray_set(back, boffset++, libxl_sprintf(ctx, PCI_BDF, pcidev->domain, pcidev->bus, pcidev->dev, pcidev->func));
        if (pcidev->vdevfn) {
            flexarray_set(back, boffset++, libxl_sprintf(ctx, "vdevfn-%d", i));
            flexarray_set(back, boffset++, libxl_sprintf(ctx, "%x", pcidev->vdevfn));
        }
        flexarray_set(back, boffset++, libxl_sprintf(ctx, "opts-%d", i));
        flexarray_set(back, boffset++, libxl_sprintf(ctx, "msitranslate=%d,power_mgmt=%d", pcidev->msitranslate, pcidev->power_mgmt));
        flexarray_set(back, boffset++, libxl_sprintf(ctx, "state-%d", i));
        flexarray_set(back, boffset++, libxl_sprintf(ctx, "%d", 1));
    }
    flexarray_set(back, boffset++, "num_devs");
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "%d", num));

    flexarray_set(front, foffset++, "backend-id");
    flexarray_set(front, foffset++, libxl_sprintf(ctx, "%d", 0));
    flexarray_set(front, foffset++, "state");
    flexarray_set(front, foffset++, libxl_sprintf(ctx, "%d", 1));

    libxl_device_generic_add(ctx, &device,
                             libxl_xs_kvs_of_flexarray(ctx, back, boffset),
                             libxl_xs_kvs_of_flexarray(ctx, front, foffset));

    flexarray_free(back);
    flexarray_free(front);
    return 0;
}

static int libxl_device_pci_add_xenstore(libxl_ctx *ctx, uint32_t domid, libxl_device_pci *pcidev)
{
    flexarray_t *back;
    char *num_devs, *be_path;
    int num = 0;
    unsigned int boffset = 0;
    xs_transaction_t t;

    be_path = libxl_sprintf(ctx, "%s/backend/pci/%d/0", libxl_xs_get_dompath(ctx, 0), domid);
    num_devs = libxl_xs_read(ctx, XBT_NULL, libxl_sprintf(ctx, "%s/num_devs", be_path));
    if (!num_devs)
        return libxl_create_pci_backend(ctx, domid, pcidev, 1);

    if (!is_hvm(ctx, domid)) {
        if (libxl_wait_for_backend(ctx, be_path, "4") < 0)
            return ERROR_FAIL;
    }

    back = flexarray_make(16, 1);
    if (!back)
        return ERROR_NOMEM;

    XL_LOG(ctx, XL_LOG_DEBUG, "Adding new pci device to xenstore");
    num = atoi(num_devs);
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "key-%d", num));
    flexarray_set(back, boffset++, libxl_sprintf(ctx, PCI_BDF, pcidev->domain, pcidev->bus, pcidev->dev, pcidev->func));
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "dev-%d", num));
    flexarray_set(back, boffset++, libxl_sprintf(ctx, PCI_BDF, pcidev->domain, pcidev->bus, pcidev->dev, pcidev->func));
    if (pcidev->vdevfn) {
        flexarray_set(back, boffset++, libxl_sprintf(ctx, "vdevfn-%d", num));
        flexarray_set(back, boffset++, libxl_sprintf(ctx, "%x", pcidev->vdevfn));
    }
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "opts-%d", num));
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "msitranslate=%d,power_mgmt=%d", pcidev->msitranslate, pcidev->power_mgmt));
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "state-%d", num));
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "%d", 1));
    flexarray_set(back, boffset++, "num_devs");
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "%d", num + 1));
    flexarray_set(back, boffset++, "state");
    flexarray_set(back, boffset++, libxl_sprintf(ctx, "%d", 7));

retry_transaction:
    t = xs_transaction_start(ctx->xsh);
    libxl_xs_writev(ctx, t, be_path,
                    libxl_xs_kvs_of_flexarray(ctx, back, boffset));
    if (!xs_transaction_end(ctx->xsh, t, 0))
        if (errno == EAGAIN)
            goto retry_transaction;

    flexarray_free(back);
    return 0;
}

static int libxl_device_pci_remove_xenstore(libxl_ctx *ctx, uint32_t domid, libxl_device_pci *pcidev)
{
    char *be_path, *num_devs_path, *num_devs, *xsdev, *tmp, *tmppath;
    int num, i, j;
    xs_transaction_t t;
    unsigned int domain = 0, bus = 0, dev = 0, func = 0;

    be_path = libxl_sprintf(ctx, "%s/backend/pci/%d/0", libxl_xs_get_dompath(ctx, 0), domid);
    num_devs_path = libxl_sprintf(ctx, "%s/num_devs", be_path);
    num_devs = libxl_xs_read(ctx, XBT_NULL, num_devs_path);
    if (!num_devs)
        return ERROR_INVAL;
    num = atoi(num_devs);

    if (!is_hvm(ctx, domid)) {
        if (libxl_wait_for_backend(ctx, be_path, "4") < 0) {
            XL_LOG(ctx, XL_LOG_DEBUG, "pci backend at %s is not ready", be_path);
            return ERROR_FAIL;
        }
    }

    for (i = 0; i < num; i++) {
        xsdev = libxl_xs_read(ctx, XBT_NULL, libxl_sprintf(ctx, "%s/dev-%d", be_path, i));
        sscanf(xsdev, PCI_BDF, &domain, &bus, &dev, &func);
        if (domain == pcidev->domain && bus == pcidev->bus &&
            pcidev->dev == dev && pcidev->func == func) {
            break;
        }
    }
    if (i == num) {
        XL_LOG(ctx, XL_LOG_ERROR, "Couldn't find the device on xenstore");
        return ERROR_INVAL;
    }

retry_transaction:
    t = xs_transaction_start(ctx->xsh);
    xs_write(ctx->xsh, t, libxl_sprintf(ctx, "%s/state-%d", be_path, i), "5", strlen("5"));
    xs_write(ctx->xsh, t, libxl_sprintf(ctx, "%s/state", be_path), "7", strlen("7"));
    if (!xs_transaction_end(ctx->xsh, t, 0))
        if (errno == EAGAIN)
            goto retry_transaction;

    if (!is_hvm(ctx, domid)) {
        if (libxl_wait_for_backend(ctx, be_path, "4") < 0) {
            XL_LOG(ctx, XL_LOG_DEBUG, "pci backend at %s is not ready", be_path);
            return ERROR_FAIL;
        }
    }

retry_transaction2:
    t = xs_transaction_start(ctx->xsh);
    xs_rm(ctx->xsh, t, libxl_sprintf(ctx, "%s/state-%d", be_path, i));
    xs_rm(ctx->xsh, t, libxl_sprintf(ctx, "%s/key-%d", be_path, i));
    xs_rm(ctx->xsh, t, libxl_sprintf(ctx, "%s/dev-%d", be_path, i));
    xs_rm(ctx->xsh, t, libxl_sprintf(ctx, "%s/vdev-%d", be_path, i));
    xs_rm(ctx->xsh, t, libxl_sprintf(ctx, "%s/opts-%d", be_path, i));
    xs_rm(ctx->xsh, t, libxl_sprintf(ctx, "%s/vdevfn-%d", be_path, i));
    libxl_xs_write(ctx, t, num_devs_path, "%d", num - 1);
    for (j = i + 1; j < num; j++) {
        tmppath = libxl_sprintf(ctx, "%s/state-%d", be_path, j);
        tmp = libxl_xs_read(ctx, t, tmppath);
        xs_write(ctx->xsh, t, libxl_sprintf(ctx, "%s/state-%d", be_path, j - 1), tmp, strlen(tmp));
        xs_rm(ctx->xsh, t, tmppath);
        tmppath = libxl_sprintf(ctx, "%s/dev-%d", be_path, j);
        tmp = libxl_xs_read(ctx, t, tmppath);
        xs_write(ctx->xsh, t, libxl_sprintf(ctx, "%s/dev-%d", be_path, j - 1), tmp, strlen(tmp));
        xs_rm(ctx->xsh, t, tmppath);
        tmppath = libxl_sprintf(ctx, "%s/key-%d", be_path, j);
        tmp = libxl_xs_read(ctx, t, tmppath);
        xs_write(ctx->xsh, t, libxl_sprintf(ctx, "%s/key-%d", be_path, j - 1), tmp, strlen(tmp));
        xs_rm(ctx->xsh, t, tmppath);
        tmppath = libxl_sprintf(ctx, "%s/vdev-%d", be_path, j);
        tmp = libxl_xs_read(ctx, t, tmppath);
        if (tmp) {
            xs_write(ctx->xsh, t, libxl_sprintf(ctx, "%s/vdev-%d", be_path, j - 1), tmp, strlen(tmp));
            xs_rm(ctx->xsh, t, tmppath);
        }
        tmppath = libxl_sprintf(ctx, "%s/opts-%d", be_path, j);
        tmp = libxl_xs_read(ctx, t, tmppath);
        if (tmp) {
            xs_write(ctx->xsh, t, libxl_sprintf(ctx, "%s/opts-%d", be_path, j - 1), tmp, strlen(tmp));
            xs_rm(ctx->xsh, t, tmppath);
        }
        tmppath = libxl_sprintf(ctx, "%s/vdevfn-%d", be_path, j);
        tmp = libxl_xs_read(ctx, t, tmppath);
        if (tmp) {
            xs_write(ctx->xsh, t, libxl_sprintf(ctx, "%s/vdevfn-%d", be_path, j - 1), tmp, strlen(tmp));
            xs_rm(ctx->xsh, t, tmppath);
        }
    }
    if (!xs_transaction_end(ctx->xsh, t, 0))
        if (errno == EAGAIN)
            goto retry_transaction2;

    if (num == 1) {
        char *fe_path = libxl_xs_read(ctx, XBT_NULL, libxl_sprintf(ctx, "%s/frontend", be_path));
        libxl_device_destroy(ctx, be_path, 1);
        xs_rm(ctx->xsh, XBT_NULL, be_path);
        xs_rm(ctx->xsh, XBT_NULL, fe_path);
        return 0;
    }

    return 0;
}

static int get_all_assigned_devices(libxl_ctx *ctx, libxl_device_pci **list, int *num)
{
    libxl_device_pci *pcidevs = NULL;
    char **domlist;
    unsigned int nd = 0, i;

    *list = NULL;
    *num = 0;

    domlist = libxl_xs_directory(ctx, XBT_NULL, "/local/domain", &nd);
    for(i = 0; i < nd; i++) {
        char *path, *num_devs;

        path = libxl_sprintf(ctx, "/local/domain/0/backend/pci/%s/0/num_devs", domlist[i]);
        num_devs = libxl_xs_read(ctx, XBT_NULL, path);
        if ( num_devs ) {
            int ndev = atoi(num_devs), j;
            char *devpath, *bdf;

            pcidevs = calloc(sizeof(*pcidevs), ndev);
            for(j = (pcidevs) ? 0 : ndev; j < ndev; j++) {
                devpath = libxl_sprintf(ctx, "/local/domain/0/backend/pci/%s/0/dev-%u",
                                        domlist[i], j);
                bdf = libxl_xs_read(ctx, XBT_NULL, devpath);
                if ( bdf ) {
                    unsigned dom, bus, dev, func;
                    if ( sscanf(bdf, PCI_BDF, &dom, &bus, &dev, &func) != 4 )
                        continue;

                    libxl_device_pci_init(pcidevs + *num, dom, bus, dev, func, 0);
                    (*num)++;
                }
            }
        }
    }

    if ( 0 == *num ) {
        free(pcidevs);
        pcidevs = NULL;
    }else{
        *list = pcidevs;
    }

    return 0;
}

static int is_assigned(libxl_device_pci *assigned, int num_assigned,
                       int dom, int bus, int dev, int func)
{
    int i;

    for(i = 0; i < num_assigned; i++) {
        if ( assigned[i].domain != dom )
            continue;
        if ( assigned[i].bus != bus )
            continue;
        if ( assigned[i].dev != dev )
            continue;
        if ( assigned[i].func != func )
            continue;
        return 1;
    }

    return 0;
}

static int do_pci_add(libxl_ctx *ctx, uint32_t domid, libxl_device_pci *pcidev)
{
    char *path;
    char *state, *vdevfn;
    int rc, hvm;

    hvm = is_hvm(ctx, domid);
    if (hvm) {
        if (libxl_wait_for_device_model(ctx, domid, "running", NULL, NULL) < 0) {
            return ERROR_FAIL;
        }
        path = libxl_sprintf(ctx, "/local/domain/0/device-model/%d/state", domid);
        state = libxl_xs_read(ctx, XBT_NULL, path);
        path = libxl_sprintf(ctx, "/local/domain/0/device-model/%d/parameter", domid);
        if (pcidev->vdevfn)
            libxl_xs_write(ctx, XBT_NULL, path, PCI_BDF_VDEVFN, pcidev->domain,
                           pcidev->bus, pcidev->dev, pcidev->func, pcidev->vdevfn);
        else
            libxl_xs_write(ctx, XBT_NULL, path, PCI_BDF, pcidev->domain,
                           pcidev->bus, pcidev->dev, pcidev->func);
        path = libxl_sprintf(ctx, "/local/domain/0/device-model/%d/command", domid);
        xs_write(ctx->xsh, XBT_NULL, path, "pci-ins", strlen("pci-ins"));
        if (libxl_wait_for_device_model(ctx, domid, "pci-inserted", NULL, NULL) < 0)
            XL_LOG(ctx, XL_LOG_ERROR, "Device Model didn't respond in time");
        path = libxl_sprintf(ctx, "/local/domain/0/device-model/%d/parameter", domid);
        vdevfn = libxl_xs_read(ctx, XBT_NULL, path);
        sscanf(vdevfn + 2, "%x", &pcidev->vdevfn);
        path = libxl_sprintf(ctx, "/local/domain/0/device-model/%d/state", domid);
        xs_write(ctx->xsh, XBT_NULL, path, state, strlen(state));
    } else {
        char *sysfs_path = libxl_sprintf(ctx, SYSFS_PCI_DEV"/"PCI_BDF"/resource", pcidev->domain,
                                         pcidev->bus, pcidev->dev, pcidev->func);
        FILE *f = fopen(sysfs_path, "r");
        unsigned long long start = 0, end = 0, flags = 0, size = 0;
        int irq = 0;
        int i;

        if (f == NULL) {
            XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "Couldn't open %s", sysfs_path);
            return ERROR_FAIL;
        }
        for (i = 0; i < PROC_PCI_NUM_RESOURCES; i++) {
            if (fscanf(f, "0x%llx 0x%llx 0x%llx\n", &start, &end, &flags) != 3)
                continue;
            size = end - start + 1;
            if (start) {
                if (flags & PCI_BAR_IO) {
                    rc = xc_domain_ioport_permission(ctx->xch, domid, start, size, 1);
                    if (rc < 0) {
                        XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "Error: xc_domain_ioport_permission error 0x%llx/0x%llx", start, size);
                        fclose(f);
                        return ERROR_FAIL;
                    }
                } else {
                    rc = xc_domain_iomem_permission(ctx->xch, domid, start>>XC_PAGE_SHIFT,
                                                    (size+(XC_PAGE_SIZE-1))>>XC_PAGE_SHIFT, 1);
                    if (rc < 0) {
                        XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "Error: xc_domain_iomem_permission error 0x%llx/0x%llx", start, size);
                        fclose(f);
                        return ERROR_FAIL;
                    }
                }
            }
        }
        fclose(f);
        sysfs_path = libxl_sprintf(ctx, SYSFS_PCI_DEV"/"PCI_BDF"/irq", pcidev->domain,
                                   pcidev->bus, pcidev->dev, pcidev->func);
        f = fopen(sysfs_path, "r");
        if (f == NULL) {
            XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "Couldn't open %s", sysfs_path);
            goto out;
        }
        if ((fscanf(f, "%u", &irq) == 1) && irq) {
            rc = xc_physdev_map_pirq(ctx->xch, domid, irq, &irq);
            if (rc < 0) {
                XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "Error: xc_physdev_map_pirq irq=%d", irq);
                fclose(f);
                return ERROR_FAIL;
            }
            rc = xc_domain_irq_permission(ctx->xch, domid, irq, 1);
            if (rc < 0) {
                XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "Error: xc_domain_irq_permission irq=%d", irq);
                fclose(f);
                return ERROR_FAIL;
            }
        }
        fclose(f);
    }
out:
    if (!libxl_is_stubdom(ctx, domid, NULL)) {
        rc = xc_assign_device(ctx->xch, domid, pcidev->value);
        if (rc < 0) {
            XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "xc_assign_device failed");
            return ERROR_FAIL;
        }
    }

    libxl_device_pci_add_xenstore(ctx, domid, pcidev);
    return 0;
}

int libxl_device_pci_add(libxl_ctx *ctx, uint32_t domid, libxl_device_pci *pcidev)
{
    libxl_device_pci *assigned;
    int num_assigned, rc;
    int stubdomid = 0;

    rc = get_all_assigned_devices(ctx, &assigned, &num_assigned);
    if ( rc ) {
        XL_LOG(ctx, XL_LOG_ERROR, "cannot determine if device is assigned, refusing to continue");
        return ERROR_FAIL;
    }
    if ( is_assigned(assigned, num_assigned, pcidev->domain,
                     pcidev->bus, pcidev->dev, pcidev->func) ) {
        XL_LOG(ctx, XL_LOG_ERROR, "PCI device already attached to a domain");
        free(assigned);
        return ERROR_FAIL;
    }
    free(assigned);

    libxl_device_pci_reset(ctx, pcidev->domain, pcidev->bus, pcidev->dev, pcidev->func);

    stubdomid = libxl_get_stubdom_id(ctx, domid);
    if (stubdomid != 0) {
        libxl_device_pci pcidev_s = *pcidev;
        rc = do_pci_add(ctx, stubdomid, &pcidev_s);
        if ( rc )
            return rc;
    }

    return do_pci_add(ctx, domid, pcidev);
}

int libxl_device_pci_remove(libxl_ctx *ctx, uint32_t domid, libxl_device_pci *pcidev)
{
    char *path;
    char *state;
    int hvm, rc;
    int stubdomid = 0;

    /* TODO: check if the device can be detached */
    libxl_device_pci_remove_xenstore(ctx, domid, pcidev);

    hvm = is_hvm(ctx, domid);
    if (hvm) {
        if (libxl_wait_for_device_model(ctx, domid, "running", NULL, NULL) < 0) {
            return ERROR_FAIL;
        }
        path = libxl_sprintf(ctx, "/local/domain/0/device-model/%d/state", domid);
        state = libxl_xs_read(ctx, XBT_NULL, path);
        path = libxl_sprintf(ctx, "/local/domain/0/device-model/%d/parameter", domid);
        libxl_xs_write(ctx, XBT_NULL, path, PCI_BDF, pcidev->domain,
                       pcidev->bus, pcidev->dev, pcidev->func);
        path = libxl_sprintf(ctx, "/local/domain/0/device-model/%d/command", domid);
        xs_write(ctx->xsh, XBT_NULL, path, "pci-rem", strlen("pci-rem"));
        if (libxl_wait_for_device_model(ctx, domid, "pci-removed", NULL, NULL) < 0) {
            XL_LOG(ctx, XL_LOG_ERROR, "Device Model didn't respond in time");
            return ERROR_FAIL;
        }
        path = libxl_sprintf(ctx, "/local/domain/0/device-model/%d/state", domid);
        xs_write(ctx->xsh, XBT_NULL, path, state, strlen(state));
    } else {
        char *sysfs_path = libxl_sprintf(ctx, SYSFS_PCI_DEV"/"PCI_BDF"/resource", pcidev->domain,
                                         pcidev->bus, pcidev->dev, pcidev->func);
        FILE *f = fopen(sysfs_path, "r");
        unsigned int start = 0, end = 0, flags = 0, size = 0;
        int irq = 0;
        int i;

        if (f == NULL) {
            XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "Couldn't open %s", sysfs_path);
            goto skip1;
        }
        for (i = 0; i < PROC_PCI_NUM_RESOURCES; i++) {
            if (fscanf(f, "0x%x 0x%x 0x%x\n", &start, &end, &flags) != 3)
                continue;
            size = end - start + 1;
            if (start) {
                if (flags & PCI_BAR_IO) {
                    rc = xc_domain_ioport_permission(ctx->xch, domid, start, size, 0);
                    if (rc < 0)
                        XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "xc_domain_ioport_permission error 0x%x/0x%x", start, size);
                } else {
                    rc = xc_domain_iomem_permission(ctx->xch, domid, start>>XC_PAGE_SHIFT,
                                                    (size+(XC_PAGE_SIZE-1))>>XC_PAGE_SHIFT, 0);
                    if (rc < 0)
                        XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "xc_domain_iomem_permission error 0x%x/0x%x", start, size);
                }
            }
        }
        fclose(f);
skip1:
        sysfs_path = libxl_sprintf(ctx, SYSFS_PCI_DEV"/"PCI_BDF"/irq", pcidev->domain,
                                   pcidev->bus, pcidev->dev, pcidev->func);
        f = fopen(sysfs_path, "r");
        if (f == NULL) {
            XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "Couldn't open %s", sysfs_path);
            goto out;
        }
        if ((fscanf(f, "%u", &irq) == 1) && irq) {
            rc = xc_physdev_unmap_pirq(ctx->xch, domid, irq);
            if (rc < 0) {
                XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "xc_physdev_map_pirq irq=%d", irq);
            }
            rc = xc_domain_irq_permission(ctx->xch, domid, irq, 0);
            if (rc < 0) {
                XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "xc_domain_irq_permission irq=%d", irq);
            }
        }
        fclose(f);
    }
out:
    libxl_device_pci_reset(ctx, pcidev->domain, pcidev->bus, pcidev->dev, pcidev->func);

    if (!libxl_is_stubdom(ctx, domid, NULL)) {
        rc = xc_deassign_device(ctx->xch, domid, pcidev->value);
        if (rc < 0)
            XL_LOG_ERRNOVAL(ctx, XL_LOG_ERROR, rc, "xc_deassign_device failed");
    }

    stubdomid = libxl_get_stubdom_id(ctx, domid);
    if (stubdomid != 0) {
        libxl_device_pci pcidev_s = *pcidev;
        libxl_device_pci_remove(ctx, stubdomid, &pcidev_s);
    }

    return 0;
}

static libxl_device_pci *scan_sys_pcidir(libxl_device_pci *assigned,
                                         int num_assigned, const char *path, int *num)
{
    libxl_device_pci *pcidevs = NULL, *new;
    struct dirent *de;
    DIR *dir;

    dir = opendir(path);
    if ( NULL == dir )
        return pcidevs;

    while( (de = readdir(dir)) ) {
        unsigned dom, bus, dev, func;
        if ( sscanf(de->d_name, PCI_BDF, &dom, &bus, &dev, &func) != 4 )
            continue;

        if ( is_assigned(assigned, num_assigned, dom, bus, dev, func) )
            continue;

        new = realloc(pcidevs, ((*num) + 1) * sizeof(*new));
        if ( NULL == new )
            continue;

        pcidevs = new;
        new = pcidevs + *num;

        memset(new, 0, sizeof(*new));
        libxl_device_pci_init(new, dom, bus, dev, func, 0);
        (*num)++;
    }

    closedir(dir);
    return pcidevs;
}

int libxl_device_pci_list_assignable(libxl_ctx *ctx, libxl_device_pci **list, int *num)
{
    libxl_device_pci *pcidevs = NULL;
    libxl_device_pci *assigned;
    int num_assigned, rc;

    *num = 0;
    *list = NULL;

    rc = get_all_assigned_devices(ctx, &assigned, &num_assigned);
    if ( rc )
        return rc;

    pcidevs = scan_sys_pcidir(assigned, num_assigned,
                              SYSFS_PCIBACK_DRIVER, num);

    free(assigned);
    if ( *num )
        *list = pcidevs;
    return 0;
}

int libxl_device_pci_list_assigned(libxl_ctx *ctx, libxl_device_pci **list, uint32_t domid, int *num)
{
    char *be_path, *num_devs, *xsdev, *xsvdevfn, *xsopts;
    int n, i;
    unsigned int domain = 0, bus = 0, dev = 0, func = 0, vdevfn = 0;
    libxl_device_pci *pcidevs;

    be_path = libxl_sprintf(ctx, "%s/backend/pci/%d/0", libxl_xs_get_dompath(ctx, 0), domid);
    num_devs = libxl_xs_read(ctx, XBT_NULL, libxl_sprintf(ctx, "%s/num_devs", be_path));
    if (!num_devs) {
        *num = 0;
        *list = NULL;
        return ERROR_FAIL;
    }
    n = atoi(num_devs);
    pcidevs = calloc(n, sizeof(libxl_device_pci));
    *num = n;

    for (i = 0; i < n; i++) {
        xsdev = libxl_xs_read(ctx, XBT_NULL, libxl_sprintf(ctx, "%s/dev-%d", be_path, i));
        sscanf(xsdev, PCI_BDF, &domain, &bus, &dev, &func);
        xsvdevfn = libxl_xs_read(ctx, XBT_NULL, libxl_sprintf(ctx, "%s/vdevfn-%d", be_path, i));
        if (xsvdevfn)
            vdevfn = strtol(xsvdevfn, (char **) NULL, 16);
        libxl_device_pci_init(pcidevs + i, domain, bus, dev, func, vdevfn);
        xsopts = libxl_xs_read(ctx, XBT_NULL, libxl_sprintf(ctx, "%s/opts-%d", be_path, i));
        if (xsopts) {
            char *saveptr;
            char *p = strtok_r(xsopts, ",=", &saveptr);
            do {
                while (*p == ' ')
                    p++;
                if (!strcmp(p, "msitranslate")) {
                    p = strtok_r(NULL, ",=", &saveptr);
                    pcidevs[i].msitranslate = atoi(p);
                } else if (!strcmp(p, "power_mgmt")) {
                    p = strtok_r(NULL, ",=", &saveptr);
                    pcidevs[i].power_mgmt = atoi(p);
                }
            } while ((p = strtok_r(NULL, ",=", &saveptr)) != NULL);
        }
    }
    if ( *num )
        *list = pcidevs;
    return 0;
}

int libxl_device_pci_shutdown(libxl_ctx *ctx, uint32_t domid)
{
    libxl_device_pci *pcidevs;
    int num, i, rc;

    rc = libxl_device_pci_list_assigned(ctx, &pcidevs, domid, &num);
    if ( rc )
        return rc;
    for (i = 0; i < num; i++) {
        if (libxl_device_pci_remove(ctx, domid, pcidevs + i) < 0)
            return ERROR_FAIL;
    }
    free(pcidevs);
    return 0;
}

int libxl_device_pci_init(libxl_device_pci *pcidev, unsigned int domain,
                          unsigned int bus, unsigned int dev,
                          unsigned int func, unsigned int vdevfn)
{
    pcidev->domain = domain;
    pcidev->bus = bus;
    pcidev->dev = dev;
    pcidev->func = func;
    pcidev->vdevfn = vdevfn;
    return 0;
}

int libxl_device_pci_reset(libxl_ctx *ctx, unsigned int domain, unsigned int bus,
                         unsigned int dev, unsigned int func)
{
    char *reset = "/sys/bus/pci/drivers/pciback/do_flr";
    int fd, rc;

    fd = open(reset, O_WRONLY);
    if (fd > 0) {
        char *buf = libxl_sprintf(ctx, PCI_BDF, domain, bus, dev, func);
        rc = write(fd, buf, strlen(buf));
        if (rc < 0)
            XL_LOG(ctx, XL_LOG_ERROR, "write to %s returned %d", reset, rc);
        close(fd);
        return rc < 0 ? rc : 0;
    }
    if (errno != ENOENT)
        XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "Failed to access pciback path %s", reset);
    reset = libxl_sprintf(ctx, "/sys/bus/pci/devices/"PCI_BDF"/reset", domain, bus, dev, func);
    fd = open(reset, O_WRONLY);
    if (fd > 0) {
        rc = write(fd, "1", 1);
        if (rc < 0)
            XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "write to %s returned %d", reset, rc);
        close(fd);
        return rc < 0 ? rc : 0;
    }
    if (errno == ENOENT) {
        XL_LOG(ctx, XL_LOG_ERROR, "The kernel doesn't support PCI device reset from sysfs");
    } else {
        XL_LOG_ERRNO(ctx, XL_LOG_ERROR, "Failed to access reset path %s", reset);
    }
    return -1;
}