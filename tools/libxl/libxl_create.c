/*
 * Copyright (C) 2010      Citrix Ltd.
 * Author Vincent Hanquez <vincent.hanquez@eu.citrix.com>
 * Author Stefano Stabellini <stefano.stabellini@eu.citrix.com>
 * Author Gianni Tedesco <gianni.tedesco@citrix.com>
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

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"

#include <xc_dom.h>
#include <xenguest.h>

void libxl_domain_config_init(libxl_domain_config *d_config)
{
    memset(d_config, 0, sizeof(*d_config));
    libxl_domain_create_info_init(&d_config->c_info);
    libxl_domain_build_info_init(&d_config->b_info);
}

void libxl_domain_config_dispose(libxl_domain_config *d_config)
{
    int i;

    for (i=0; i<d_config->num_disks; i++)
        libxl_device_disk_dispose(&d_config->disks[i]);
    free(d_config->disks);

    for (i=0; i<d_config->num_vifs; i++)
        libxl_device_nic_dispose(&d_config->vifs[i]);
    free(d_config->vifs);

    for (i=0; i<d_config->num_pcidevs; i++)
        libxl_device_pci_dispose(&d_config->pcidevs[i]);
    free(d_config->pcidevs);

    for (i=0; i<d_config->num_vfbs; i++)
        libxl_device_vfb_dispose(&d_config->vfbs[i]);
    free(d_config->vfbs);

    for (i=0; i<d_config->num_vkbs; i++)
        libxl_device_vkb_dispose(&d_config->vkbs[i]);
    free(d_config->vkbs);

    libxl_domain_create_info_dispose(&d_config->c_info);
    libxl_domain_build_info_dispose(&d_config->b_info);
}

int libxl__domain_create_info_setdefault(libxl__gc *gc,
                                         libxl_domain_create_info *c_info)
{
    if (!c_info->type)
        return ERROR_INVAL;

    if (c_info->type == LIBXL_DOMAIN_TYPE_HVM) {
        libxl_defbool_setdefault(&c_info->hap, true);
        libxl_defbool_setdefault(&c_info->oos, true);
    }

    return 0;
}

int libxl__domain_build_info_setdefault(libxl__gc *gc,
                                        libxl_domain_build_info *b_info)
{
    if (b_info->type != LIBXL_DOMAIN_TYPE_HVM &&
        b_info->type != LIBXL_DOMAIN_TYPE_PV)
        return ERROR_INVAL;

    libxl_defbool_setdefault(&b_info->device_model_stubdomain, false);

    if (!b_info->device_model_version) {
        if (b_info->type == LIBXL_DOMAIN_TYPE_HVM)
            b_info->device_model_version =
                LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL;
        else {
            const char *dm;
            int rc;

            b_info->device_model_version =
                LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN;
            dm = libxl__domain_device_model(gc, b_info);
            rc = access(dm, X_OK);
            if (rc < 0) {
                /* qemu-xen unavailable, use qemu-xen-traditional */
                if (errno == ENOENT) {
                    LIBXL__LOG_ERRNO(CTX, XTL_VERBOSE, "qemu-xen is unavailable"
                            ", use qemu-xen-traditional instead");
                    b_info->device_model_version =
                        LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL;
                } else {
                    LIBXL__LOG_ERRNO(CTX, XTL_ERROR, "qemu-xen access error");
                    return ERROR_FAIL;
                }
            }
        }
    }

    if (b_info->type == LIBXL_DOMAIN_TYPE_HVM) {
        if (!b_info->u.hvm.bios)
            switch (b_info->device_model_version) {
            case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL:
                b_info->u.hvm.bios = LIBXL_BIOS_TYPE_ROMBIOS; break;
            case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN:
                b_info->u.hvm.bios = LIBXL_BIOS_TYPE_SEABIOS; break;
            default:return ERROR_INVAL;
            }

        /* Enforce BIOS<->Device Model version relationship */
        switch (b_info->device_model_version) {
        case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL:
            if (b_info->u.hvm.bios != LIBXL_BIOS_TYPE_ROMBIOS)
                return ERROR_INVAL;
            break;
        case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN:
            if (b_info->u.hvm.bios == LIBXL_BIOS_TYPE_ROMBIOS)
                return ERROR_INVAL;
            break;
        default:abort();
        }
    }

    if (b_info->type == LIBXL_DOMAIN_TYPE_HVM &&
        b_info->device_model_version !=
            LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL &&
        libxl_defbool_val(b_info->device_model_stubdomain)) {
        LIBXL__LOG(CTX, XTL_ERROR,
            "device model stubdomains require \"qemu-xen-traditional\"");
        return ERROR_INVAL;
    }

    if (!b_info->max_vcpus)
        b_info->max_vcpus = 1;
    if (!b_info->cur_vcpus)
        b_info->cur_vcpus = 1;

    if (!b_info->cpumap.size) {
        if (libxl_cpumap_alloc(CTX, &b_info->cpumap))
            return ERROR_NOMEM;
        libxl_cpumap_set_any(&b_info->cpumap);
    }

    if (b_info->max_memkb == LIBXL_MEMKB_DEFAULT)
        b_info->max_memkb = 32 * 1024;
    if (b_info->target_memkb == LIBXL_MEMKB_DEFAULT)
        b_info->target_memkb = b_info->max_memkb;

    libxl_defbool_setdefault(&b_info->localtime, false);

    libxl_defbool_setdefault(&b_info->disable_migrate, false);

    switch (b_info->type) {
    case LIBXL_DOMAIN_TYPE_HVM:
        if (b_info->shadow_memkb == LIBXL_MEMKB_DEFAULT)
            b_info->shadow_memkb = 0;
        if (b_info->video_memkb == LIBXL_MEMKB_DEFAULT)
            b_info->video_memkb = 8 * 1024;
        if (b_info->u.hvm.timer_mode == LIBXL_TIMER_MODE_DEFAULT)
            b_info->u.hvm.timer_mode =
                LIBXL_TIMER_MODE_NO_DELAY_FOR_MISSED_TICKS;

        libxl_defbool_setdefault(&b_info->u.hvm.pae,                true);
        libxl_defbool_setdefault(&b_info->u.hvm.apic,               true);
        libxl_defbool_setdefault(&b_info->u.hvm.acpi,               true);
        libxl_defbool_setdefault(&b_info->u.hvm.acpi_s3,            true);
        libxl_defbool_setdefault(&b_info->u.hvm.acpi_s4,            true);
        libxl_defbool_setdefault(&b_info->u.hvm.nx,                 true);
        libxl_defbool_setdefault(&b_info->u.hvm.viridian,           false);
        libxl_defbool_setdefault(&b_info->u.hvm.hpet,               true);
        libxl_defbool_setdefault(&b_info->u.hvm.vpt_align,          true);
        libxl_defbool_setdefault(&b_info->u.hvm.nested_hvm,         false);
        libxl_defbool_setdefault(&b_info->u.hvm.incr_generationid,  false);
        libxl_defbool_setdefault(&b_info->u.hvm.usb,                false);
        libxl_defbool_setdefault(&b_info->u.hvm.xen_platform_pci,   true);

        if (!b_info->u.hvm.boot) {
            b_info->u.hvm.boot = strdup("cda");
            if (!b_info->u.hvm.boot) return ERROR_NOMEM;
        }

        libxl_defbool_setdefault(&b_info->u.hvm.stdvga, false);
        libxl_defbool_setdefault(&b_info->u.hvm.vnc.enable, true);
        if (libxl_defbool_val(b_info->u.hvm.vnc.enable)) {
            libxl_defbool_setdefault(&b_info->u.hvm.vnc.findunused, true);
            if (!b_info->u.hvm.vnc.listen) {
                b_info->u.hvm.vnc.listen = strdup("127.0.0.1");
                if (!b_info->u.hvm.vnc.listen) return ERROR_NOMEM;
            }
        }

        libxl_defbool_setdefault(&b_info->u.hvm.sdl.enable, false);
        if (libxl_defbool_val(b_info->u.hvm.sdl.enable)) {
            libxl_defbool_setdefault(&b_info->u.hvm.sdl.opengl, false);
        }

        libxl_defbool_setdefault(&b_info->u.hvm.spice.enable, false);
        if (libxl_defbool_val(b_info->u.hvm.spice.enable)) {
            libxl_defbool_setdefault(&b_info->u.hvm.spice.disable_ticketing,
                                     false);
            libxl_defbool_setdefault(&b_info->u.hvm.spice.agent_mouse, true);
        }

        libxl_defbool_setdefault(&b_info->u.hvm.nographic, false);

        libxl_defbool_setdefault(&b_info->u.hvm.gfx_passthru, false);

        break;
    case LIBXL_DOMAIN_TYPE_PV:
        libxl_defbool_setdefault(&b_info->u.pv.e820_host, false);
        if (b_info->shadow_memkb == LIBXL_MEMKB_DEFAULT)
            b_info->shadow_memkb = 0;
        if (b_info->u.pv.slack_memkb == LIBXL_MEMKB_DEFAULT)
            b_info->u.pv.slack_memkb = 0;
        break;
    default:
        LIBXL__LOG(CTX, LIBXL__LOG_ERROR,
                   "invalid domain type %s in create info",
                   libxl_domain_type_to_string(b_info->type));
        return ERROR_INVAL;
    }
    return 0;
}

static int init_console_info(libxl__device_console *console, int dev_num)
{
    memset(console, 0x00, sizeof(libxl__device_console));
    console->devid = dev_num;
    console->consback = LIBXL__CONSOLE_BACKEND_XENCONSOLED;
    console->output = strdup("pty");
    if (!console->output)
        return ERROR_NOMEM;
    return 0;
}
int libxl__domain_build(libxl__gc *gc,
                        libxl_domain_build_info *info,
                        uint32_t domid,
                        libxl__domain_build_state *state)
{
    char **vments = NULL, **localents = NULL;
    struct timeval start_time;
    int i, ret;

    ret = libxl__build_pre(gc, domid, info, state);
    if (ret)
        goto out;

    gettimeofday(&start_time, NULL);

    switch (info->type) {
    case LIBXL_DOMAIN_TYPE_HVM:
        ret = libxl__build_hvm(gc, domid, info, state);
        if (ret)
            goto out;

        vments = libxl__calloc(gc, 7, sizeof(char *));
        vments[0] = "rtc/timeoffset";
        vments[1] = (info->u.hvm.timeoffset) ? info->u.hvm.timeoffset : "";
        vments[2] = "image/ostype";
        vments[3] = "hvm";
        vments[4] = "start_time";
        vments[5] = libxl__sprintf(gc, "%lu.%02d", start_time.tv_sec,(int)start_time.tv_usec/10000);

        localents = libxl__calloc(gc, 7, sizeof(char *));
        localents[0] = "platform/acpi";
        localents[1] = libxl_defbool_val(info->u.hvm.acpi) ? "1" : "0";
        localents[2] = "platform/acpi_s3";
        localents[3] = libxl_defbool_val(info->u.hvm.acpi_s3) ? "1" : "0";
        localents[4] = "platform/acpi_s4";
        localents[5] = libxl_defbool_val(info->u.hvm.acpi_s4) ? "1" : "0";

        break;
    case LIBXL_DOMAIN_TYPE_PV:
        ret = libxl__build_pv(gc, domid, info, state);
        if (ret)
            goto out;

        vments = libxl__calloc(gc, 11, sizeof(char *));
        i = 0;
        vments[i++] = "image/ostype";
        vments[i++] = "linux";
        vments[i++] = "image/kernel";
        vments[i++] = (char*) info->u.pv.kernel.path;
        vments[i++] = "start_time";
        vments[i++] = libxl__sprintf(gc, "%lu.%02d", start_time.tv_sec,(int)start_time.tv_usec/10000);
        if (info->u.pv.ramdisk.path) {
            vments[i++] = "image/ramdisk";
            vments[i++] = (char*) info->u.pv.ramdisk.path;
        }
        if (info->u.pv.cmdline) {
            vments[i++] = "image/cmdline";
            vments[i++] = (char*) info->u.pv.cmdline;
        }
        break;
    default:
        ret = ERROR_INVAL;
        goto out;
    }
    ret = libxl__build_post(gc, domid, info, state, vments, localents);
out:
    return ret;
}

static int domain_restore(libxl__gc *gc, libxl_domain_build_info *info,
                          uint32_t domid, int fd,
                          libxl__domain_build_state *state)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    char **vments = NULL, **localents = NULL;
    struct timeval start_time;
    int i, ret, esave, flags;

    ret = libxl__build_pre(gc, domid, info, state);
    if (ret)
        goto out;

    ret = libxl__domain_restore_common(gc, domid, info, state, fd);
    if (ret)
        goto out;

    gettimeofday(&start_time, NULL);

    switch (info->type) {
    case LIBXL_DOMAIN_TYPE_HVM:
        vments = libxl__calloc(gc, 7, sizeof(char *));
        vments[0] = "rtc/timeoffset";
        vments[1] = (info->u.hvm.timeoffset) ? info->u.hvm.timeoffset : "";
        vments[2] = "image/ostype";
        vments[3] = "hvm";
        vments[4] = "start_time";
        vments[5] = libxl__sprintf(gc, "%lu.%02d", start_time.tv_sec,(int)start_time.tv_usec/10000);
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        vments = libxl__calloc(gc, 11, sizeof(char *));
        i = 0;
        vments[i++] = "image/ostype";
        vments[i++] = "linux";
        vments[i++] = "image/kernel";
        vments[i++] = (char*) info->u.pv.kernel.path;
        vments[i++] = "start_time";
        vments[i++] = libxl__sprintf(gc, "%lu.%02d", start_time.tv_sec,(int)start_time.tv_usec/10000);
        if (info->u.pv.ramdisk.path) {
            vments[i++] = "image/ramdisk";
            vments[i++] = (char*) info->u.pv.ramdisk.path;
        }
        if (info->u.pv.cmdline) {
            vments[i++] = "image/cmdline";
            vments[i++] = (char*) info->u.pv.cmdline;
        }
        break;
    default:
        ret = ERROR_INVAL;
        goto out;
    }
    ret = libxl__build_post(gc, domid, info, state, vments, localents);
    if (ret)
        goto out;

    if (info->type == LIBXL_DOMAIN_TYPE_HVM) {
        ret = asprintf(&state->saved_state,
                       XC_DEVICE_MODEL_RESTORE_FILE".%d", domid);
        ret = (ret < 0) ? ERROR_FAIL : 0;
    }

out:
    if (info->type == LIBXL_DOMAIN_TYPE_PV) {
        libxl__file_reference_unmap(&info->u.pv.kernel);
        libxl__file_reference_unmap(&info->u.pv.ramdisk);
    }

    esave = errno;

    flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "unable to get flags on restore fd");
    } else {
        flags &= ~O_NONBLOCK;
        if (fcntl(fd, F_SETFL, flags) == -1)
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "unable to put restore fd"
                         " back to blocking mode");
    }

    errno = esave;
    return ret;
}

int libxl__domain_make(libxl__gc *gc, libxl_domain_create_info *info,
                       uint32_t *domid)
 /* on entry, libxl_domid_valid_guest(domid) must be false;
  * on exit (even error exit), domid may be valid and refer to a domain */
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    int flags, ret, rc;
    char *uuid_string;
    char *dom_path, *vm_path, *libxl_path;
    struct xs_permissions roperm[2];
    struct xs_permissions rwperm[1];
    struct xs_permissions noperm[1];
    xs_transaction_t t = 0;
    xen_domain_handle_t handle;


    assert(!libxl_domid_valid_guest(*domid));

    uuid_string = libxl__uuid2string(gc, info->uuid);
    if (!uuid_string) {
        rc = ERROR_NOMEM;
        goto out;
    }

    flags = 0;
    if (info->type == LIBXL_DOMAIN_TYPE_HVM) {
        flags |= XEN_DOMCTL_CDF_hvm_guest;
        flags |= libxl_defbool_val(info->hap) ? XEN_DOMCTL_CDF_hap : 0;
        flags |= libxl_defbool_val(info->oos) ? 0 : XEN_DOMCTL_CDF_oos_off;
    }
    *domid = -1;

    /* Ultimately, handle is an array of 16 uint8_t, same as uuid */
    libxl_uuid_copy((libxl_uuid *)handle, &info->uuid);

    ret = xc_domain_create(ctx->xch, info->ssidref, handle, flags, domid);
    if (ret < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, ret, "domain creation fail");
        rc = ERROR_FAIL;
        goto out;
    }

    ret = xc_cpupool_movedomain(ctx->xch, info->poolid, *domid);
    if (ret < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, ret, "domain move fail");
        rc = ERROR_FAIL;
        goto out;
    }

    dom_path = libxl__xs_get_dompath(gc, *domid);
    if (!dom_path) {
        rc = ERROR_FAIL;
        goto out;
    }

    vm_path = libxl__sprintf(gc, "/vm/%s", uuid_string);
    if (!vm_path) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "cannot allocate create paths");
        rc = ERROR_FAIL;
        goto out;
    }

    libxl_path = libxl__xs_libxl_path(gc, *domid);
    if (!libxl_path) {
        rc = ERROR_FAIL;
        goto out;
    }

    noperm[0].id = 0;
    noperm[0].perms = XS_PERM_NONE;

    roperm[0].id = 0;
    roperm[0].perms = XS_PERM_NONE;
    roperm[1].id = *domid;
    roperm[1].perms = XS_PERM_READ;

    rwperm[0].id = *domid;
    rwperm[0].perms = XS_PERM_NONE;

retry_transaction:
    t = xs_transaction_start(ctx->xsh);

    xs_rm(ctx->xsh, t, dom_path);
    libxl__xs_mkdir(gc, t, dom_path, roperm, ARRAY_SIZE(roperm));

    xs_rm(ctx->xsh, t, vm_path);
    libxl__xs_mkdir(gc, t, vm_path, roperm, ARRAY_SIZE(roperm));

    xs_rm(ctx->xsh, t, libxl_path);
    libxl__xs_mkdir(gc, t, libxl_path, noperm, ARRAY_SIZE(noperm));

    xs_write(ctx->xsh, t, libxl__sprintf(gc, "%s/vm", dom_path), vm_path, strlen(vm_path));
    rc = libxl__domain_rename(gc, *domid, 0, info->name, t);
    if (rc)
        goto out;

    libxl__xs_mkdir(gc, t,
                    libxl__sprintf(gc, "%s/cpu", dom_path),
                    roperm, ARRAY_SIZE(roperm));
    libxl__xs_mkdir(gc, t,
                    libxl__sprintf(gc, "%s/memory", dom_path),
                    roperm, ARRAY_SIZE(roperm));
    libxl__xs_mkdir(gc, t,
                    libxl__sprintf(gc, "%s/device", dom_path),
                    roperm, ARRAY_SIZE(roperm));
    libxl__xs_mkdir(gc, t,
                    libxl__sprintf(gc, "%s/control", dom_path),
                    roperm, ARRAY_SIZE(roperm));
    if (info->type == LIBXL_DOMAIN_TYPE_HVM)
        libxl__xs_mkdir(gc, t,
                        libxl__sprintf(gc, "%s/hvmloader", dom_path),
                        roperm, ARRAY_SIZE(roperm));

    libxl__xs_mkdir(gc, t,
                    libxl__sprintf(gc, "%s/control/shutdown", dom_path),
                    rwperm, ARRAY_SIZE(rwperm));
    libxl__xs_mkdir(gc, t,
                    libxl__sprintf(gc, "%s/device/suspend/event-channel", dom_path),
                    rwperm, ARRAY_SIZE(rwperm));
    libxl__xs_mkdir(gc, t,
                    libxl__sprintf(gc, "%s/data", dom_path),
                    rwperm, ARRAY_SIZE(rwperm));
    if (info->type == LIBXL_DOMAIN_TYPE_HVM)
        libxl__xs_mkdir(gc, t,
            libxl__sprintf(gc, "%s/hvmloader/generation-id-address", dom_path),
                        rwperm, ARRAY_SIZE(rwperm));

    xs_write(ctx->xsh, t, libxl__sprintf(gc, "%s/uuid", vm_path), uuid_string, strlen(uuid_string));
    xs_write(ctx->xsh, t, libxl__sprintf(gc, "%s/name", vm_path), info->name, strlen(info->name));

    libxl__xs_writev(gc, t, dom_path, info->xsdata);
    libxl__xs_writev(gc, t, libxl__sprintf(gc, "%s/platform", dom_path), info->platformdata);

    xs_write(ctx->xsh, t, libxl__sprintf(gc, "%s/control/platform-feature-multiprocessor-suspend", dom_path), "1", 1);
    xs_write(ctx->xsh, t, libxl__sprintf(gc, "%s/control/platform-feature-xs_reset_watches", dom_path), "1", 1);
    if (!xs_transaction_end(ctx->xsh, t, 0)) {
        if (errno == EAGAIN) {
            t = 0;
            goto retry_transaction;
        }
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "domain creation "
                         "xenstore transaction commit failed");
        rc = ERROR_FAIL;
        goto out;
    }
    t = 0;

    rc = 0;
 out:
    if (t) xs_transaction_end(ctx->xsh, t, 1);
    return rc;
}

static int store_libxl_entry(libxl__gc *gc, uint32_t domid,
                             libxl_domain_build_info *b_info)
{
    char *path = NULL;

    path = libxl__xs_libxl_path(gc, domid);
    path = libxl__sprintf(gc, "%s/dm-version", path);
    return libxl__xs_write(gc, XBT_NULL, path, "%s",
        libxl_device_model_version_to_string(b_info->device_model_version));
}

static int do_domain_create(libxl__gc *gc, libxl_domain_config *d_config,
                            libxl_console_ready cb, void *priv,
                            uint32_t *domid_out, int restore_fd)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    libxl__spawner_starting *dm_starting = 0;
    libxl__domain_build_state state;
    uint32_t domid;
    int i, ret;

    domid = 0;

    ret = libxl__domain_create_info_setdefault(gc, &d_config->c_info);
    if (ret) goto error_out;

    ret = libxl__domain_create_info_setdefault(gc, &d_config->c_info);
    if (ret) goto error_out;

    ret = libxl__domain_make(gc, &d_config->c_info, &domid);
    if (ret) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "cannot make domain: %d", ret);
        ret = ERROR_FAIL;
        goto error_out;
    }

    if ( d_config->c_info.type == LIBXL_DOMAIN_TYPE_PV && cb ) {
        if ( (*cb)(ctx, domid, priv) )
            goto error_out;
    }

    ret = libxl__domain_build_info_setdefault(gc, &d_config->b_info);
    if (ret) goto error_out;

    for (i = 0; i < d_config->num_disks; i++) {
        ret = libxl__device_disk_setdefault(gc, &d_config->disks[i]);
        if (ret) goto error_out;
    }

    if ( restore_fd < 0 ) {
        ret = libxl_run_bootloader(ctx, &d_config->b_info, d_config->num_disks > 0 ? &d_config->disks[0] : NULL, domid);
        if (ret) {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                       "failed to run bootloader: %d", ret);
            goto error_out;
        }
    }

    memset(&state, 0, sizeof(state));

    if ( restore_fd >= 0 ) {
        ret = domain_restore(gc, &d_config->b_info, domid, restore_fd, &state);
    } else {
        ret = libxl__domain_build(gc, &d_config->b_info, domid, &state);
    }

    if (ret) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "cannot (re-)build domain: %d", ret);
        ret = ERROR_FAIL;
        goto error_out;
    }

    store_libxl_entry(gc, domid, &d_config->b_info);

    for (i = 0; i < d_config->num_disks; i++) {
        ret = libxl_device_disk_add(ctx, domid, &d_config->disks[i]);
        if (ret) {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                       "cannot add disk %d to domain: %d", i, ret);
            ret = ERROR_FAIL;
            goto error_out;
        }
    }
    for (i = 0; i < d_config->num_vifs; i++) {
        ret = libxl_device_nic_add(ctx, domid, &d_config->vifs[i]);
        if (ret) {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                       "cannot add nic %d to domain: %d", i, ret);
            ret = ERROR_FAIL;
            goto error_out;
        }
    }
    switch (d_config->c_info.type) {
    case LIBXL_DOMAIN_TYPE_HVM:
    {
        libxl__device_console console;
        libxl_device_vkb vkb;

        ret = init_console_info(&console, 0);
        if ( ret )
            goto error_out;
        libxl__device_console_add(gc, domid, &console, &state);
        libxl__device_console_dispose(&console);

        libxl_device_vkb_init(&vkb);
        libxl_device_vkb_add(ctx, domid, &vkb);
        libxl_device_vkb_dispose(&vkb);

        ret = libxl__create_device_model(gc, domid, d_config,
                                         &state, &dm_starting);
        if (ret < 0) {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                       "failed to create device model: %d", ret);
            goto error_out;
        }
        break;
    }
    case LIBXL_DOMAIN_TYPE_PV:
    {
        int need_qemu = 0;
        libxl__device_console console;

        for (i = 0; i < d_config->num_vfbs; i++) {
            libxl_device_vfb_add(ctx, domid, &d_config->vfbs[i]);
            libxl_device_vkb_add(ctx, domid, &d_config->vkbs[i]);
        }

        ret = init_console_info(&console, 0);
        if ( ret )
            goto error_out;

        need_qemu = libxl__need_xenpv_qemu(gc, 1, &console,
                d_config->num_vfbs, d_config->vfbs,
                d_config->num_disks, &d_config->disks[0]);

        if (need_qemu)
             console.consback = LIBXL__CONSOLE_BACKEND_IOEMU;

        libxl__device_console_add(gc, domid, &console, &state);
        libxl__device_console_dispose(&console);

        if (need_qemu) {
            libxl__create_xenpv_qemu(gc, domid, d_config, &state, &dm_starting);
        }
        break;
    }
    default:
        ret = ERROR_INVAL;
        goto error_out;
    }

    if (dm_starting) {
        if (d_config->b_info.device_model_version
            == LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN) {
            libxl__qmp_initializations(gc, domid, d_config);
        }
        ret = libxl__confirm_device_model_startup(gc, &state, dm_starting);
        if (ret < 0) {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                       "device model did not start: %d", ret);
            goto error_out;
        }
    }

    for (i = 0; i < d_config->num_pcidevs; i++)
        libxl__device_pci_add(gc, domid, &d_config->pcidevs[i], 1);

    if (d_config->num_pcidevs > 0) {
        ret = libxl__create_pci_backend(gc, domid, d_config->pcidevs,
            d_config->num_pcidevs);
        if (ret < 0) {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                "libxl_create_pci_backend failed: %d", ret);
            goto error_out;
        }
    }

    if (d_config->c_info.type == LIBXL_DOMAIN_TYPE_PV &&
        libxl_defbool_val(d_config->b_info.u.pv.e820_host)) {
        int rc;
        rc = libxl__e820_alloc(gc, domid, d_config);
        if (rc)
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                      "Failed while collecting E820 with: %d (errno:%d)\n",
                      rc, errno);
    }
    if ( cb && (d_config->c_info.type == LIBXL_DOMAIN_TYPE_HVM ||
                (d_config->c_info.type == LIBXL_DOMAIN_TYPE_PV &&
                 d_config->b_info.u.pv.bootloader ))) {
        if ( (*cb)(ctx, domid, priv) )
            goto error_out;
    }

    *domid_out = domid;
    return 0;

error_out:
    if (domid)
        libxl_domain_destroy(ctx, domid);

    return ret;
}

int libxl_domain_create_new(libxl_ctx *ctx, libxl_domain_config *d_config,
                            libxl_console_ready cb, void *priv, uint32_t *domid)
{
    GC_INIT(ctx);
    int rc;
    rc = do_domain_create(gc, d_config, cb, priv, domid, -1);
    GC_FREE;
    return rc;
}

int libxl_domain_create_restore(libxl_ctx *ctx, libxl_domain_config *d_config,
                                libxl_console_ready cb, void *priv, uint32_t *domid, int restore_fd)
{
    GC_INIT(ctx);
    int rc;
    rc = do_domain_create(gc, d_config, cb, priv, domid, restore_fd);
    GC_FREE;
    return rc;
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
