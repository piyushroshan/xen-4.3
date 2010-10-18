/******************************************************************************
 * xc_misc.c
 *
 * Miscellaneous control interface functions.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "xc_private.h"
#include <xen/hvm/hvm_op.h>

int xc_readconsolering(xc_interface *xch,
                       char *buffer,
                       unsigned int *pnr_chars,
                       int clear, int incremental, uint32_t *pindex)
{
    int ret;
    DECLARE_SYSCTL;
    unsigned int nr_chars = *pnr_chars;

    sysctl.cmd = XEN_SYSCTL_readconsole;
    set_xen_guest_handle(sysctl.u.readconsole.buffer, buffer);
    sysctl.u.readconsole.count = nr_chars;
    sysctl.u.readconsole.clear = clear;
    sysctl.u.readconsole.incremental = 0;
    if ( pindex )
    {
        sysctl.u.readconsole.index = *pindex;
        sysctl.u.readconsole.incremental = incremental;
    }

    if ( (ret = lock_pages(xch, buffer, nr_chars)) != 0 )
        return ret;

    if ( (ret = do_sysctl(xch, &sysctl)) == 0 )
    {
        *pnr_chars = sysctl.u.readconsole.count;
        if ( pindex )
            *pindex = sysctl.u.readconsole.index;
    }

    unlock_pages(xch, buffer, nr_chars);

    return ret;
}

int xc_send_debug_keys(xc_interface *xch, char *keys)
{
    int ret, len = strlen(keys);
    DECLARE_SYSCTL;

    sysctl.cmd = XEN_SYSCTL_debug_keys;
    set_xen_guest_handle(sysctl.u.debug_keys.keys, keys);
    sysctl.u.debug_keys.nr_keys = len;

    if ( (ret = lock_pages(xch, keys, len)) != 0 )
        return ret;

    ret = do_sysctl(xch, &sysctl);

    unlock_pages(xch, keys, len);

    return ret;
}

int xc_physinfo(xc_interface *xch,
                xc_physinfo_t *put_info)
{
    int ret;
    DECLARE_SYSCTL;

    sysctl.cmd = XEN_SYSCTL_physinfo;

    memcpy(&sysctl.u.physinfo, put_info, sizeof(*put_info));

    if ( (ret = do_sysctl(xch, &sysctl)) != 0 )
        return ret;

    memcpy(put_info, &sysctl.u.physinfo, sizeof(*put_info));

    return 0;
}

int xc_topologyinfo(xc_interface *xch,
                xc_topologyinfo_t *put_info)
{
    int ret;
    DECLARE_SYSCTL;

    sysctl.cmd = XEN_SYSCTL_topologyinfo;

    memcpy(&sysctl.u.topologyinfo, put_info, sizeof(*put_info));

    if ( (ret = do_sysctl(xch, &sysctl)) != 0 )
        return ret;

    memcpy(put_info, &sysctl.u.topologyinfo, sizeof(*put_info));

    return 0;
}

int xc_numainfo(xc_interface *xch,
                xc_numainfo_t *put_info)
{
    int ret;
    DECLARE_SYSCTL;

    sysctl.cmd = XEN_SYSCTL_numainfo;

    memcpy(&sysctl.u.numainfo, put_info, sizeof(*put_info));

    if ((ret = do_sysctl(xch, &sysctl)) != 0)
        return ret;

    memcpy(put_info, &sysctl.u.numainfo, sizeof(*put_info));

    return 0;
}


int xc_sched_id(xc_interface *xch,
                int *sched_id)
{
    int ret;
    DECLARE_SYSCTL;

    sysctl.cmd = XEN_SYSCTL_sched_id;

    if ( (ret = do_sysctl(xch, &sysctl)) != 0 )
        return ret;

    *sched_id = sysctl.u.sched_id.sched_id;

    return 0;
}

#if defined(__i386__) || defined(__x86_64__)
int xc_mca_op(xc_interface *xch, struct xen_mc *mc)
{
    int ret = 0;
    DECLARE_HYPERCALL;

    mc->interface_version = XEN_MCA_INTERFACE_VERSION;
    if ( lock_pages(xch, mc, sizeof(mc)) )
    {
        PERROR("Could not lock xen_mc memory");
        return -EINVAL;
    }

    hypercall.op = __HYPERVISOR_mca;
    hypercall.arg[0] = (unsigned long)mc;
    ret = do_xen_hypercall(xch, &hypercall);
    unlock_pages(xch, mc, sizeof(mc));
    return ret;
}
#endif

int xc_perfc_control(xc_interface *xch,
                     uint32_t opcode,
                     xc_perfc_desc_t *desc,
                     xc_perfc_val_t *val,
                     int *nbr_desc,
                     int *nbr_val)
{
    int rc;
    DECLARE_SYSCTL;

    sysctl.cmd = XEN_SYSCTL_perfc_op;
    sysctl.u.perfc_op.cmd = opcode;
    set_xen_guest_handle(sysctl.u.perfc_op.desc, desc);
    set_xen_guest_handle(sysctl.u.perfc_op.val, val);

    rc = do_sysctl(xch, &sysctl);

    if ( nbr_desc )
        *nbr_desc = sysctl.u.perfc_op.nr_counters;
    if ( nbr_val )
        *nbr_val = sysctl.u.perfc_op.nr_vals;

    return rc;
}

int xc_lockprof_control(xc_interface *xch,
                        uint32_t opcode,
                        uint32_t *n_elems,
                        uint64_t *time,
                        xc_lockprof_data_t *data)
{
    int rc;
    DECLARE_SYSCTL;

    sysctl.cmd = XEN_SYSCTL_lockprof_op;
    sysctl.u.lockprof_op.cmd = opcode;
    sysctl.u.lockprof_op.max_elem = n_elems ? *n_elems : 0;
    set_xen_guest_handle(sysctl.u.lockprof_op.data, data);

    rc = do_sysctl(xch, &sysctl);

    if (n_elems)
        *n_elems = sysctl.u.lockprof_op.nr_elem;
    if (time)
        *time = sysctl.u.lockprof_op.time;

    return rc;
}

int xc_getcpuinfo(xc_interface *xch, int max_cpus,
                  xc_cpuinfo_t *info, int *nr_cpus)
{
    int rc;
    DECLARE_SYSCTL;

    sysctl.cmd = XEN_SYSCTL_getcpuinfo;
    sysctl.u.getcpuinfo.max_cpus = max_cpus; 
    set_xen_guest_handle(sysctl.u.getcpuinfo.info, info); 

    if ( (rc = lock_pages(xch, info, max_cpus*sizeof(*info))) != 0 )
        return rc;

    rc = do_sysctl(xch, &sysctl);

    unlock_pages(xch, info, max_cpus*sizeof(*info));

    if ( nr_cpus )
        *nr_cpus = sysctl.u.getcpuinfo.nr_cpus; 

    return rc;
}


int xc_hvm_set_pci_intx_level(
    xc_interface *xch, domid_t dom,
    uint8_t domain, uint8_t bus, uint8_t device, uint8_t intx,
    unsigned int level)
{
    DECLARE_HYPERCALL;
    struct xen_hvm_set_pci_intx_level _arg, *arg = &_arg;
    int rc;

    if ( (rc = hcall_buf_prep(xch, (void **)&arg, sizeof(*arg))) != 0 )
    {
        PERROR("Could not lock memory");
        return rc;
    }

    hypercall.op     = __HYPERVISOR_hvm_op;
    hypercall.arg[0] = HVMOP_set_pci_intx_level;
    hypercall.arg[1] = (unsigned long)arg;

    arg->domid  = dom;
    arg->domain = domain;
    arg->bus    = bus;
    arg->device = device;
    arg->intx   = intx;
    arg->level  = level;

    rc = do_xen_hypercall(xch, &hypercall);

    hcall_buf_release(xch, (void **)&arg, sizeof(*arg));

    return rc;
}

int xc_hvm_set_isa_irq_level(
    xc_interface *xch, domid_t dom,
    uint8_t isa_irq,
    unsigned int level)
{
    DECLARE_HYPERCALL;
    struct xen_hvm_set_isa_irq_level _arg, *arg = &_arg;
    int rc;

    if ( (rc = hcall_buf_prep(xch, (void **)&arg, sizeof(*arg))) != 0 )
    {
        PERROR("Could not lock memory");
        return rc;
    }

    hypercall.op     = __HYPERVISOR_hvm_op;
    hypercall.arg[0] = HVMOP_set_isa_irq_level;
    hypercall.arg[1] = (unsigned long)arg;

    arg->domid   = dom;
    arg->isa_irq = isa_irq;
    arg->level   = level;

    rc = do_xen_hypercall(xch, &hypercall);

    hcall_buf_release(xch, (void **)&arg, sizeof(*arg));

    return rc;
}

int xc_hvm_set_pci_link_route(
    xc_interface *xch, domid_t dom, uint8_t link, uint8_t isa_irq)
{
    DECLARE_HYPERCALL;
    struct xen_hvm_set_pci_link_route arg;
    int rc;

    hypercall.op     = __HYPERVISOR_hvm_op;
    hypercall.arg[0] = HVMOP_set_pci_link_route;
    hypercall.arg[1] = (unsigned long)&arg;

    arg.domid   = dom;
    arg.link    = link;
    arg.isa_irq = isa_irq;

    if ( (rc = lock_pages(xch, &arg, sizeof(arg))) != 0 )
    {
        PERROR("Could not lock memory");
        return rc;
    }

    rc = do_xen_hypercall(xch, &hypercall);

    unlock_pages(xch, &arg, sizeof(arg));

    return rc;
}

int xc_hvm_track_dirty_vram(
    xc_interface *xch, domid_t dom,
    uint64_t first_pfn, uint64_t nr,
    unsigned long *dirty_bitmap)
{
    DECLARE_HYPERCALL;
    struct xen_hvm_track_dirty_vram arg;
    int rc;

    hypercall.op     = __HYPERVISOR_hvm_op;
    hypercall.arg[0] = HVMOP_track_dirty_vram;
    hypercall.arg[1] = (unsigned long)&arg;

    arg.domid     = dom;
    arg.first_pfn = first_pfn;
    arg.nr        = nr;
    set_xen_guest_handle(arg.dirty_bitmap, (uint8_t *)dirty_bitmap);

    if ( (rc = lock_pages(xch, &arg, sizeof(arg))) != 0 )
    {
        PERROR("Could not lock memory");
        return rc;
    }

    rc = do_xen_hypercall(xch, &hypercall);

    unlock_pages(xch, &arg, sizeof(arg));

    return rc;
}

int xc_hvm_modified_memory(
    xc_interface *xch, domid_t dom, uint64_t first_pfn, uint64_t nr)
{
    DECLARE_HYPERCALL;
    struct xen_hvm_modified_memory arg;
    int rc;

    hypercall.op     = __HYPERVISOR_hvm_op;
    hypercall.arg[0] = HVMOP_modified_memory;
    hypercall.arg[1] = (unsigned long)&arg;

    arg.domid     = dom;
    arg.first_pfn = first_pfn;
    arg.nr        = nr;

    if ( (rc = lock_pages(xch, &arg, sizeof(arg))) != 0 )
    {
        PERROR("Could not lock memory");
        return rc;
    }

    rc = do_xen_hypercall(xch, &hypercall);

    unlock_pages(xch, &arg, sizeof(arg));

    return rc;
}

int xc_hvm_set_mem_type(
    xc_interface *xch, domid_t dom, hvmmem_type_t mem_type, uint64_t first_pfn, uint64_t nr)
{
    DECLARE_HYPERCALL;
    struct xen_hvm_set_mem_type arg;
    int rc;

    hypercall.op     = __HYPERVISOR_hvm_op;
    hypercall.arg[0] = HVMOP_set_mem_type;
    hypercall.arg[1] = (unsigned long)&arg;

    arg.domid        = dom;
    arg.hvmmem_type  = mem_type;
    arg.first_pfn    = first_pfn;
    arg.nr           = nr;

    if ( (rc = lock_pages(xch, &arg, sizeof(arg))) != 0 )
    {
        PERROR("Could not lock memory");
        return rc;
    }

    rc = do_xen_hypercall(xch, &hypercall);

    unlock_pages(xch, &arg, sizeof(arg));

    return rc;
}


/* stub for all not yet converted OSes */
void *
#ifdef __GNUC__
__attribute__((__weak__))
#endif
xc_map_foreign_bulk(xc_interface *xch, uint32_t dom, int prot,
                    const xen_pfn_t *arr, int *err, unsigned int num)
{
    xen_pfn_t *pfn;
    unsigned int i;
    void *ret;

    if ((int)num <= 0) {
        errno = EINVAL;
        return NULL;
    }

    pfn = malloc(num * sizeof(*pfn));
    if (!pfn) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(pfn, arr, num * sizeof(*arr));
    ret = xc_map_foreign_batch(xch, dom, prot, pfn, num);

    if (ret) {
        for (i = 0; i < num; ++i)
            switch (pfn[i] ^ arr[i]) {
            case 0:
                err[i] = 0;
                break;
            default:
                err[i] = -EINVAL;
                break;
            }
    } else
        memset(err, 0, num * sizeof(*err));

    free(pfn);

    return ret;
}

void *xc_map_foreign_pages(xc_interface *xch, uint32_t dom, int prot,
                           const xen_pfn_t *arr, int num)
{
    void *res;
    int i, *err;

    if (num < 0) {
        errno = -EINVAL;
        return NULL;
    }

    err = malloc(num * sizeof(*err));
    if (!err)
        return NULL;

    res = xc_map_foreign_bulk(xch, dom, prot, arr, err, num);
    if (res) {
        for (i = 0; i < num; i++) {
            if (err[i]) {
                errno = -err[i];
                munmap(res, num * PAGE_SIZE);
                res = NULL;
                break;
            }
        }
    }

    free(err);
    return res;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
