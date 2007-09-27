/*
 * intr.c: handling I/O, interrupts related VMX entry/exit
 * Copyright (c) 2004, Intel Corporation.
 * Copyright (c) 2004-2007, XenSource Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/trace.h>
#include <xen/event.h>
#include <asm/current.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/io.h>
#include <asm/hvm/support.h>
#include <asm/hvm/vmx/vmx.h>
#include <asm/hvm/vmx/vmcs.h>
#include <asm/hvm/vpic.h>
#include <asm/hvm/vlapic.h>
#include <public/hvm/ioreq.h>
#include <asm/hvm/trace.h>

/*
 * A few notes on virtual NMI and INTR delivery, and interactions with
 * interruptibility states:
 * 
 * We can only inject an ExtInt if EFLAGS.IF = 1 and no blocking by
 * STI nor MOV SS. Otherwise the VM entry fails. The 'virtual interrupt
 * pending' control causes a VM exit when all these checks succeed. It will
 * exit immediately after VM entry if the checks succeed at that point.
 * 
 * We can only inject an NMI if no blocking by MOV SS (also, depending on
 * implementation, if no blocking by STI). If pin-based 'virtual NMIs'
 * control is specified then the NMI-blocking interruptibility flag is
 * also checked. The 'virtual NMI pending' control (available only in
 * conjunction with 'virtual NMIs') causes a VM exit when all these checks
 * succeed. It will exit immediately after VM entry if the checks succeed
 * at that point.
 * 
 * Because a processor may or may not check blocking-by-STI when injecting
 * a virtual NMI, it will be necessary to convert that to block-by-MOV-SS
 * before specifying the 'virtual NMI pending' control. Otherwise we could
 * enter an infinite loop where we check blocking-by-STI in software and
 * thus delay delivery of a virtual NMI, but the processor causes immediate
 * VM exit because it does not check blocking-by-STI.
 * 
 * Injecting a virtual NMI sets the NMI-blocking interruptibility flag only
 * if the 'virtual NMIs' control is set. Injecting *any* kind of event clears
 * the STI- and MOV-SS-blocking interruptibility-state flags.
 * 
 * If MOV/POP SS is executed while MOV-SS-blocking is in effect, the effect
 * is cleared. If STI is executed while MOV-SS- or STI-blocking is in effect,
 * the effect is cleared. (i.e., MOV-SS-blocking 'dominates' STI-blocking).
 */

static void enable_intr_window(struct vcpu *v, enum hvm_intack intr_source)
{
    u32 *cpu_exec_control = &v->arch.hvm_vmx.exec_control;
    u32 ctl = CPU_BASED_VIRTUAL_INTR_PENDING;

    ASSERT(intr_source != hvm_intack_none);

    if ( (intr_source == hvm_intack_nmi) && cpu_has_vmx_vnmi )
    {
        /*
         * We set MOV-SS blocking in lieu of STI blocking when delivering an
         * NMI. This is because it is processor-specific whether STI-blocking
         * blocks NMIs. Hence we *must* check for STI-blocking on NMI delivery
         * (otherwise vmentry will fail on processors that check for STI-
         * blocking) but if the processor does not check for STI-blocking then
         * we may immediately vmexit and hance make no progress!
         * (see SDM 3B 21.3, "Other Causes of VM Exits").
         */
        u32 intr_shadow = __vmread(GUEST_INTERRUPTIBILITY_INFO);
        if ( intr_shadow & VMX_INTR_SHADOW_STI )
        {
            /* Having both STI-blocking and MOV-SS-blocking fails vmentry. */
            intr_shadow &= ~VMX_INTR_SHADOW_STI;
            intr_shadow |= VMX_INTR_SHADOW_MOV_SS;
            __vmwrite(GUEST_INTERRUPTIBILITY_INFO, intr_shadow);
        }
        ctl = CPU_BASED_VIRTUAL_NMI_PENDING;
    }

    if ( !(*cpu_exec_control & ctl) )
    {
        *cpu_exec_control |= ctl;
        __vmwrite(CPU_BASED_VM_EXEC_CONTROL, *cpu_exec_control);
    }
}

static void update_tpr_threshold(
    struct vcpu *v, enum hvm_intack masked_intr_source)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    int max_irr, tpr, threshold = 0;

    if ( !cpu_has_vmx_tpr_shadow )
        return;

    /*
     * If ExtInts are masked then that dominates the TPR --- the 'interrupt
     * window' has already been enabled in this case.
     */
    if ( (masked_intr_source == hvm_intack_lapic) ||
         (masked_intr_source == hvm_intack_pic) )
        goto out;

    /* Is there an interrupt pending at the LAPIC? Nothing to do if not. */
    if ( !vlapic_enabled(vlapic) || 
         ((max_irr = vlapic_find_highest_irr(vlapic)) == -1) )
        goto out;

    /* Highest-priority pending interrupt is masked by the TPR? */
    tpr = vlapic_get_reg(vlapic, APIC_TASKPRI) & 0xF0;
    if ( (tpr >> 4) >= (max_irr >> 4) )
        threshold = max_irr >> 4;

 out:
    __vmwrite(TPR_THRESHOLD, threshold);
}

static void vmx_dirq_assist(struct domain *d)
{
    unsigned int irq;
    uint32_t device, intx;
    struct hvm_irq *hvm_irq = &d->arch.hvm_domain.irq;

    for ( irq = find_first_bit(hvm_irq->dirq_mask, NR_IRQS);
          irq < NR_IRQS;
          irq = find_next_bit(hvm_irq->dirq_mask, NR_IRQS, irq + 1) )
    {
        test_and_clear_bit(irq, &hvm_irq->dirq_mask);
        device = hvm_irq->mirq[irq].device;
        intx = hvm_irq->mirq[irq].intx;
        hvm_pci_intx_assert(d, device, intx);
    }
}

asmlinkage void vmx_intr_assist(void)
{
    int intr_vector;
    enum hvm_intack intr_source;
    struct vcpu *v = current;
    unsigned int intr_info;

    /* Crank the handle on interrupt state. */
    pt_update_irq(v);

    if ( vtd_enabled && (v->vcpu_id == 0) )
        vmx_dirq_assist(v->domain);
  
    hvm_set_callback_irq_level();

    do {
        intr_source = hvm_vcpu_has_pending_irq(v);
        if ( likely(intr_source == hvm_intack_none) )
            goto out;

        /*
         * An event is already pending or the pending interrupt is masked?
         * Then the pending interrupt must be delayed.
         */
        intr_info = __vmread(VM_ENTRY_INTR_INFO);
        if ( unlikely(intr_info & INTR_INFO_VALID_MASK) ||
             !hvm_interrupts_enabled(v, intr_source) )
        {
            enable_intr_window(v, intr_source);
            goto out;
        }
    } while ( !hvm_vcpu_ack_pending_irq(v, intr_source, &intr_vector) );

    if ( intr_source == hvm_intack_nmi )
    {
        vmx_inject_nmi(v);
    }
    else
    {
        HVMTRACE_2D(INJ_VIRQ, v, intr_vector, /*fake=*/ 0);
        vmx_inject_extint(v, intr_vector);
        pt_intr_post(v, intr_vector, intr_source);
    }

    /* Is there another IRQ to queue up behind this one? */
    intr_source = hvm_vcpu_has_pending_irq(v);
    if ( unlikely(intr_source != hvm_intack_none) )
        enable_intr_window(v, intr_source);

 out:
    update_tpr_threshold(v, intr_source);
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
