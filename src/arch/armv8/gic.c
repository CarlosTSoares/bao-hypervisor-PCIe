/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#include <arch/gic.h>

#if (GIC_VERSION == GICV2)
#include <arch/gicv2.h>
#elif (GIC_VERSION == GICV3)
#include <arch/gicv3.h>
#elif (GIC_VERSION == GICV4)
#include <arch/gicv3.h>
#include <arch/gicv4.h>
#else
#error "unknown GIC version " GIC_VERSION
#endif

#include <interrupts.h>
#include <cpu.h>
#include <spinlock.h>
#include <platform.h>
#include <fences.h>

volatile struct gicd_hw* gicd;
spinlock_t gicd_lock;

void gicd_init()
{
    size_t int_num = gic_num_irqs();

    /* Bring distributor to known state */
    for (size_t i = GIC_NUM_PRIVINT_REGS; i < GIC_NUM_INT_REGS(int_num); i++) {
        /**
         * Make sure all interrupts are not enabled, non pending, non active.
         */
        gicd->IGROUPR[i] = -1;
        gicd->ICENABLER[i] = -1;
        gicd->ICPENDR[i] = -1;
        gicd->ICACTIVER[i] = -1;
    }

    /* All interrupts have lowest priority possible by default */
    for (size_t i = GIC_NUM_PRIO_REGS(GIC_CPU_PRIV); i < GIC_NUM_PRIO_REGS(int_num); i++) {
        gicd->IPRIORITYR[i] = -1;
    }

    if (GIC_VERSION == GICV2) {
        /* No CPU targets for any interrupt by default */
        for (size_t i = GIC_NUM_TARGET_REGS(GIC_CPU_PRIV); i < GIC_NUM_TARGET_REGS(int_num); i++) {
            gicd->ITARGETSR[i] = 0;
        }

        /* Enable distributor */
        gicd->CTLR |= GICD_CTLR_EN_BIT;

    } else {
        for (size_t i = GIC_CPU_PRIV; i < GIC_MAX_INTERUPTS; i++) {
            gicd->IROUTER[i] = GICD_IROUTER_INV;
        }

        /* Enable distributor and affinity routing */
        gicd->CTLR |= GICD_CTLR_ARE_NS_BIT | GICD_CTLR_ENA_BIT;
    }

    /* ICFGR are platform dependent, lets leave them as is */

    /* No need to setup gicd->NSACR as all interrupts are  setup to group 1 */

    if (!interrupts_reserve(platform.arch.gic.maintenance_id, gic_maintenance_handler)) {
        ERROR("Failed to reserve GIC maintenance interrupt");
    }
}

void gic_map_mmio();

bool gicd_supports_LPIs(){

    return (gicd->TYPER & GICD_TYPER_LPIS_BIT) ? true: false;
}



void gic_init()
{
    if (GIC_VERSION == GICV3 || GIC_VERSION == GICV4) {
        sysreg_icc_sre_el2_write(ICC_SRE_SRE_BIT | ICC_SRE_ENB_BIT);    //Enable the system register interface and enable el1 access to icc_sre_el1
        ISB();
    }

    if (cpu()->id == CPU_MASTER) {
        gic_map_mmio();
        gicd_init();
        NUM_LRS = gich_num_lrs();

        if(gicd_supports_LPIs()) //don't need gic version condition
        {
            gits_map_mmio();
            its_init();
        }
    }

    cpu_sync_and_clear_msgs(&cpu_glb_sync);

    gic_cpu_init();

}

void gic_handle()
{
    uint32_t ack = gicc_iar();  //ack
    irqid_t id = bit32_extract(ack, GICC_IAR_ID_OFF, GICC_IAR_ID_LEN);

    if (id < GIC_FIRST_SPECIAL_INTID || (id >= GIC_FIRST_LPIS && id <= GIC_MAX_LPIS)) {
        enum irq_res res = interrupts_handle(id);
        gicc_eoir(ack);         //gic end of interrupt
        if (res == HANDLED_BY_HYP) {
            gicc_dir(ack);      //gic desactivate interrupt
        }
    }
}

uint8_t gicd_get_prio(irqid_t int_id)
{
    size_t reg_ind = GIC_PRIO_REG(int_id);
    size_t off = GIC_PRIO_OFF(int_id);

    uint8_t prio = gicd->IPRIORITYR[reg_ind] >> off & BIT32_MASK(off, GIC_PRIO_BITS);

    return prio;
}

void gicd_set_icfgr(irqid_t int_id, uint8_t cfg)
{
    size_t reg_ind = (int_id * GIC_CONFIG_BITS) / (sizeof(uint32_t) * 8);
    size_t off = (int_id * GIC_CONFIG_BITS) % (sizeof(uint32_t) * 8);
    uint32_t mask = ((1U << GIC_CONFIG_BITS) - 1) << off;

    spin_lock(&gicd_lock);

    gicd->ICFGR[reg_ind] = (gicd->ICFGR[reg_ind] & ~mask) | ((cfg << off) & mask);

    spin_unlock(&gicd_lock);
}

void gicd_set_prio(irqid_t int_id, uint8_t prio)
{
    size_t reg_ind = GIC_PRIO_REG(int_id);
    size_t off = GIC_PRIO_OFF(int_id);
    uint32_t mask = BIT32_MASK(off, GIC_PRIO_BITS);

    spin_lock(&gicd_lock);

    gicd->IPRIORITYR[reg_ind] = (gicd->IPRIORITYR[reg_ind] & ~mask) | ((prio << off) & mask);

    spin_unlock(&gicd_lock);
}

void gicd_set_pend(irqid_t int_id, bool pend)
{
    size_t reg_ind = GIC_INT_REG(int_id);
    if (pend) {
        gicd->ISPENDR[reg_ind] = GIC_INT_MASK(int_id);
    } else {
        gicd->ICPENDR[reg_ind] = GIC_INT_MASK(int_id);
    }
}

bool gicd_get_pend(irqid_t int_id)
{
    return (gicd->ISPENDR[GIC_INT_REG(int_id)] & GIC_INT_MASK(int_id)) != 0;
}

void gicd_set_act(irqid_t int_id, bool act)
{
    size_t reg_ind = GIC_INT_REG(int_id);

    if (act) {
        gicd->ISACTIVER[reg_ind] = GIC_INT_MASK(int_id);
    } else {
        gicd->ICACTIVER[reg_ind] = GIC_INT_MASK(int_id);
    }
}

bool gicd_get_act(irqid_t int_id)
{
    return (gicd->ISACTIVER[GIC_INT_REG(int_id)] & GIC_INT_MASK(int_id)) != 0;
}

void gicd_set_enable(irqid_t int_id, bool en)
{
    size_t reg_ind = GIC_INT_REG(int_id);
    uint32_t bit = GIC_INT_MASK(int_id);

    if (en) {
        gicd->ISENABLER[reg_ind] = bit;
    } else {
        gicd->ICENABLER[reg_ind] = bit;
    }
}

void its_encode_cmd(struct its_cmd *cmd, uint8_t cmd_id){
    its_mask_encode(&cmd->cmd[0],cmd_id,0,8);
}

void its_encode_valid(struct its_cmd *cmd, size_t val){
    its_mask_encode(&cmd->cmd[2],val,63,1); 
}
void its_encode_target(struct its_cmd *cmd, uint64_t target){
    its_mask_encode(&cmd->cmd[2],target,16,36);  
}
void its_encode_ic_id(struct its_cmd *cmd, uint64_t ic_id){
    its_mask_encode(&cmd->cmd[2],ic_id,0,12);
}

void its_encode_device_id(struct its_cmd *cmd, uint32_t dev_id)
{
    its_mask_encode(&cmd->cmd[0],dev_id,32,32);
}

void its_encode_size(struct its_cmd *cmd, size_t size){
    its_mask_encode(&cmd->cmd[1],size,0,5);
}

void its_encode_itt_addr(struct its_cmd *cmd, uint64_t itt_addr)
{
    its_mask_encode(&cmd->cmd[2],itt_addr,0,52);
}

void its_encode_pint_id(struct its_cmd *cmd, uint32_t pint_id)
{
    its_mask_encode(&cmd->cmd[1],pint_id,32,32);
}

/*ITSv4 support*/
void its_encode_vpe_id(struct its_cmd *cmd, uint16_t vpe_id){
    its_mask_encode(&cmd->cmd[1],vpe_id,32,16); 
}
void its_encode_vpt_addr(struct its_cmd *cmd, uint64_t vpt_addr){
    its_mask_encode(&cmd->cmd[3],(vpt_addr>>16),16,36); 
}
void its_encode_vpt_size(struct its_cmd *cmd, uint8_t vpt_sz){
    its_mask_encode(&cmd->cmd[3],vpt_sz-1,0,5);
}
void its_encode_event_id(struct its_cmd *cmd, uint32_t event_id)
{
    its_mask_encode(&cmd->cmd[1],event_id,0,32);
}
void its_encode_db_id(struct its_cmd *cmd, uint32_t db_id)
{
    its_mask_encode(&cmd->cmd[2],db_id,32,32);
}
void its_encode_virt_id(struct its_cmd *cmd, uint32_t virt_id)
{
    its_mask_encode(&cmd->cmd[2],virt_id,0,32);
}
