/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#include <arch/vgic.h>
#include <arch/vgicv3.h>

#include <bit.h>
#include <spinlock.h>
#include <cpu.h>
#include <interrupts.h>
#include <vm.h>
#include <platform.h>

#define GICR_IS_REG(REG, offset)                    \
    (((offset) >= offsetof(struct gicr_hw, REG)) && \
        (offset) < (offsetof(struct gicr_hw, REG) + sizeof(gicr[0].REG)))


#define GICR_REG_OFF(REG)   (offsetof(struct gicr_hw, REG) & 0x1ffff)


#define GICR_REG_MASK(ADDR) ((ADDR) & 0x1ffff)


#define GICD_REG_MASK(ADDR) ((ADDR) & (GIC_VERSION == GICV2 ? 0xfffUL : 0xffffUL))


#define GITS_IS_REG(REG, offset)                    \
    (((offset) >= offsetof(struct gits_hw, REG)) && \
        (offset) < (offsetof(struct gits_hw, REG) + sizeof(gits->REG)))
#define GITS_REG_OFF(REG)   (offsetof(struct gits_hw, REG) & 0x1ffff)
#define GITS_REG_MASK(ADDR) ((ADDR) & 0x1ffff)

#define GITS_CMD_MASK(CMD)  (bit64_extract(CMD->cmd[0],ITS_CMD_ENC_OFF,ITS_CMD_ENC_LEN))


//static spinlock_t gits_lock = SPINLOCK_INITVAL;

bool vgic_int_has_other_target(struct vcpu* vcpu, struct vgic_int* interrupt)
{
    bool priv = gic_is_priv(interrupt->id);
    bool routed_here =
        !priv && !(interrupt->phys.route ^ (sysreg_mpidr_el1_read() & MPIDR_AFF_MSK));
    bool route_valid = interrupt->phys.route != GICD_IROUTER_INV;
    bool any = !priv && vgic_broadcast(vcpu, interrupt);
    return any || (!routed_here && route_valid);
}

uint8_t vgic_int_ptarget_mask(struct vcpu* vcpu, struct vgic_int* interrupt)
{
    if (vgic_broadcast(vcpu, interrupt)) {
        return cpu()->vcpu->vm->cpus & ~(1U << cpu()->vcpu->phys_id);
    } else {
        return (1 << interrupt->phys.route);
    }
}

bool vgic_int_set_route(struct vcpu* vcpu, struct vgic_int* interrupt, unsigned long route)
{
    unsigned long phys_route;
    unsigned long prev_route = interrupt->route;

    if (gic_is_priv(interrupt->id)) {
        return false;
    }

    if (route & GICD_IROUTER_IRM_BIT) {
        phys_route = cpu_id_to_mpidr(vcpu->phys_id);
    } else {
        struct vcpu* tvcpu = vm_get_vcpu_by_mpidr(vcpu->vm, route & MPIDR_AFF_MSK);
        if (tvcpu != NULL) {
            phys_route = cpu_id_to_mpidr(tvcpu->phys_id) & MPIDR_AFF_MSK;
        } else {
            phys_route = GICD_IROUTER_INV;
        }
    }
    interrupt->phys.route = phys_route;

    interrupt->route = route & GICD_IROUTER_RES0_MSK;
    return prev_route != interrupt->route;
}

unsigned long vgic_int_get_route(struct vcpu* vcpu, struct vgic_int* interrupt)
{
    if (gic_is_priv(interrupt->id)) {
        return 0;
    }
    return interrupt->route;
}

void vgic_int_set_route_hw(struct vcpu* vcpu, struct vgic_int* interrupt)
{
    gicd_set_route(interrupt->id, interrupt->phys.route);
}

void vgicr_emul_ctrl_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id)
{
    if (!acc->write) {
        if(cpu()->vcpu->vm->msi){
            cpuid_t pgicr_id = vm_translate_to_pcpuid(cpu()->vcpu->vm, vgicr_id);
            vcpu_writereg(cpu()->vcpu, acc->reg, gicr_get_en_lpis(pgicr_id));
        } else {
            vcpu_writereg(cpu()->vcpu, acc->reg, 0); //read as zero
        }
    } else {    //if hasnt given the msi config then the VM cannot modify the CTLR.ENABLEBITS
        if(cpu()->vcpu->vm->msi){
            cpuid_t pgicr_id = vm_translate_to_pcpuid(cpu()->vcpu->vm, vgicr_id);
            uint32_t val = gicr[pgicr_id].CTLR & ~(GICR_CTLR_EN_LPIS_MSK);
            gicr[pgicr_id].CTLR = val | (vcpu_readreg(cpu()->vcpu, acc->reg) & GICR_CTLR_EN_LPIS_MSK); 
        }
    }
}

void vgicr_emul_typer_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id)
{
    bool word_access = (acc->width == 4);
    bool top_access = word_access && ((acc->addr & 0x4) != 0);

    if (!acc->write) {
        struct vcpu* vcpu = vm_get_vcpu(cpu()->vcpu->vm, vgicr_id);
        uint64_t typer = vcpu->arch.vgic_priv.vgicr.TYPER;

        if (top_access) {
            typer >>= 32;
        } else if (word_access) {
            typer &= BIT_MASK(0, 32);
        }

        vcpu_writereg(cpu()->vcpu, acc->reg, typer);
    }
}

void vgicr_emul_pidr_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id)
{
    if (!acc->write) {
        vcpu_writereg(cpu()->vcpu, acc->reg, 0x3b);
    }
}

void vgicd_emul_router_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id)
{
    bool word_access = (acc->width == 4);
    bool top_access = word_access && ((acc->addr & 0x4) != 0);
    vaddr_t aligned_addr = acc->addr & ~((vaddr_t)0x7);
    size_t irq_id = (GICD_REG_MASK(aligned_addr) - offsetof(struct gicd_hw, IROUTER)) / 8;

    struct vgic_int* interrupt = vgic_get_int(cpu()->vcpu, irq_id, cpu()->vcpu->id);

    if (interrupt == NULL) {
        return vgic_emul_razwi(acc, handlers, gicr_access, vgicr_id);
    }

    uint64_t route = vgic_int_get_route(cpu()->vcpu, interrupt);
    if (!acc->write) {  //read route
        if (top_access) {
            vcpu_writereg(cpu()->vcpu, acc->reg, (uint32_t)(route >> 32));
        } else if (word_access) {
            vcpu_writereg(cpu()->vcpu, acc->reg, (uint32_t)route);
        } else {
            vcpu_writereg(cpu()->vcpu, acc->reg, route);
        }
    } else {
        uint64_t reg_value = vcpu_readreg(cpu()->vcpu, acc->reg);
        if (top_access) {
            route = (route & BIT64_MASK(0, 32)) | ((reg_value & BIT64_MASK(0, 32)) << 32);
        } else if (word_access) {
            route = (route & BIT64_MASK(32, 32)) | (reg_value & BIT64_MASK(0, 32));
        } else {
            route = reg_value;
        }
        vgic_int_set_field(handlers, cpu()->vcpu, interrupt, route);
    }
}

/* Propbaser and Pendbaser emulation*/

void vgicr_emul_propbaser_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id) 
{
    struct vcpu *target_vcpu = vm_get_vcpu(cpu()->vcpu->vm, vgicr_id);
    struct vm *vm = target_vcpu->vm;

    if (!acc->write) {
        
        if(vm->msi)
        {
            cpuid_t pgicr_id = vm_translate_to_pcpuid(cpu()->vcpu->vm, vgicr_id);
            if (pgicr_id != INVALID_CPUID) {
                vcpu_writereg(cpu()->vcpu, acc->reg,cpu()->vcpu->arch.vgic_priv.vgicr.PROPBASER);
            }
        } else {
            vcpu_writereg(cpu()->vcpu, acc->reg,0);
        }

    }else{
        cpuid_t pgicr_id = vm_translate_to_pcpuid(vm, vgicr_id);
        
        if((pgicr_id != INVALID_CPUID) && vm->msi && !gicr_get_en_lpis(pgicr_id))
        {
            //translate to physical
            uint64_t tmp_propbaser = vcpu_readreg(cpu()->vcpu, acc->reg);
            paddr_t proptable_pa = 0;

            vaddr_t *proptable_vaddr = (vaddr_t *)(tmp_propbaser & GICR_PROPBASER_PHY_ADDR_MSK);
            size_t id_bits = tmp_propbaser & GICR_PROPBASER_ID_BITS_MSK;
            size_t proptable_size = GICR_PROPTABLE_SZ(id_bits);

            mem_guest_ipa_translate(proptable_vaddr,&proptable_pa);


            if(vm->arch.prop_tab.proptab_base == NULL) {

                vm->arch.prop_tab.proptab_size = proptable_size;
                vm->arch.prop_tab.proptab_base = (uint8_t *)mem_alloc_map_dev(&cpu()->as, SEC_HYP_GLOBAL, INVALID_VA,
                proptable_pa,NUM_PAGES(vm->arch.prop_tab.proptab_size)); //Wrong section probably

                vm->arch.prop_tab.vm_proptable_vaddr = (vaddr_t)proptable_vaddr;

            } else {

                if((vaddr_t)proptable_vaddr != vm->arch.prop_tab.vm_proptable_vaddr) {
                    //unmap and map as RW the proptable current region in VM space

                    //unmap of proptable in Bao space
                    mem_unmap(&cpu()->as,(vaddr_t)vm->arch.prop_tab.proptab_base,vm->arch.prop_tab.proptab_size,true);


                    //map of the new proptable region in Bao Space
                    vm->arch.prop_tab.proptab_size = proptable_size;
                    vm->arch.prop_tab.proptab_base = (uint8_t *)mem_alloc_map_dev(&cpu()->as, SEC_HYP_GLOBAL, INVALID_VA,
                    proptable_pa,NUM_PAGES(vm->arch.prop_tab.proptab_size));

                    vm->arch.prop_tab.vm_proptable_vaddr = (vaddr_t)proptable_vaddr;
                } else {
                    console_printk("[BAO-VGICV3] Propbaser modification not done\n");
                }
            }

            
            target_vcpu->arch.vgic_priv.vgicr.PROPBASER = tmp_propbaser;
            
            #if (GIC_VERSION == GICV4)
                gicr_set_vpropbaser(pgicr_id,proptable_pa,id_bits);
            #else
                gicr_set_propbaser(pgicr_id,proptable_pa,id_bits);
            #endif
        }
    }
}

void vgicr_emul_pendbaser_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id) 
{
    if (!acc->write) {
        if(cpu()->vcpu->vm->msi)
        {
            cpuid_t pgicr_id = vm_translate_to_pcpuid(cpu()->vcpu->vm, vgicr_id);

            if (pgicr_id != INVALID_CPUID) {
                vcpu_writereg(cpu()->vcpu, acc->reg,cpu()->vcpu->arch.vgic_priv.vgicr.PENDBASER);
            }
        } else {
            vcpu_writereg(cpu()->vcpu, acc->reg,0);
        }

    }else {
        cpuid_t pgicr_id = vm_translate_to_pcpuid(cpu()->vcpu->vm, vgicr_id);

        if((pgicr_id != INVALID_CPUID) && cpu()->vcpu->vm->msi && !gicr_get_en_lpis(pgicr_id))
        {
            //translate to physical
            paddr_t pend_pa=0;
            uint64_t tmp = vcpu_readreg(cpu()->vcpu, acc->reg);
            vaddr_t *pendbaser_vaddr = (vaddr_t *)(tmp & GICR_PENDBASER_PHY_ADDR_MSK);

            mem_guest_ipa_translate(pendbaser_vaddr,&pend_pa);

            // uint64_t pendbaser_paddr = pend_pa |
            //             (tmp & ~GICR_PENDBASER_PHY_ADDR_MSK);
            struct vcpu *target_vcpu = vm_get_vcpu(cpu()->vcpu->vm, vgicr_id);
            target_vcpu->arch.vgic_priv.vgicr.PENDBASER = tmp;

            #if (GIC_VERSION == GICV4)
                gicr_set_vpendbaser(pgicr_id,pend_pa);
            #else
                gicr_set_pendbaser(pgicr_id,pend_pa);
            #endif    
        }
    }
}

extern struct vgic_reg_handler_info isenabler_info;
extern struct vgic_reg_handler_info ispendr_info;
extern struct vgic_reg_handler_info isactiver_info;
extern struct vgic_reg_handler_info icenabler_info;
extern struct vgic_reg_handler_info icpendr_info;
extern struct vgic_reg_handler_info iactiver_info;
extern struct vgic_reg_handler_info icfgr_info;
extern struct vgic_reg_handler_info ipriorityr_info;
extern struct vgic_reg_handler_info razwi_info;

struct vgic_reg_handler_info irouter_info = {
    vgicd_emul_router_access,
    0b1100,
    VGIC_IROUTER_ID,
    offsetof(struct gicd_hw, IROUTER),
    64,
    vgic_int_get_route,
    vgic_int_set_route,
    vgic_int_set_route_hw,
};

struct vgic_reg_handler_info vgicr_ctrl_info = {
    vgicr_emul_ctrl_access,
    0b0100,
};
struct vgic_reg_handler_info vgicr_typer_info = {
    vgicr_emul_typer_access,
    0b1100,
};
struct vgic_reg_handler_info vgicr_pidr_info = {
    vgicr_emul_pidr_access,
    0b0100,
};

struct vgic_reg_handler_info vgicr_propbaser_info = {
    vgicr_emul_propbaser_access,
    0b1000,
};

struct vgic_reg_handler_info vgicr_pendbaser_info = {
    vgicr_emul_pendbaser_access,
    0b1000,
};

static inline vcpuid_t vgicr_get_id(struct emul_access* acc)
{
    return (acc->addr - cpu()->vcpu->vm->arch.vgicr_addr) / (sizeof(struct gicr_hw)-GICR_VFRAME_SIZE);
}

bool vgicr_emul_handler(struct emul_access* acc)
{
    struct vgic_reg_handler_info* handler_info = NULL;

    switch (GICR_REG_MASK(acc->addr)) {
        case GICR_REG_OFF(CTLR):
            handler_info = &vgicr_ctrl_info;
            break;
        case GICR_REG_OFF(ISENABLER0):
            handler_info = &isenabler_info;
            break;
        case GICR_REG_OFF(ISPENDR0):
            handler_info = &ispendr_info;
            break;
        case GICR_REG_OFF(ISACTIVER0):
            handler_info = &iactiver_info;
            break;
        case GICR_REG_OFF(ICENABLER0):
            handler_info = &icenabler_info;
            break;
        case GICR_REG_OFF(ICPENDR0):
            handler_info = &icpendr_info;
            break;
        case GICR_REG_OFF(ICACTIVER0):
            handler_info = &icfgr_info;
            break;
        case GICR_REG_OFF(ICFGR0):
        case GICR_REG_OFF(ICFGR1):
            handler_info = &icfgr_info;
            break;
        case GICR_REG_OFF(PROPBASER):
            handler_info = &vgicr_propbaser_info;
            break;
        case GICR_REG_OFF(PENDBASER):
            handler_info = &vgicr_pendbaser_info;
            break;
        default: {
            size_t base_offset = acc->addr - cpu()->vcpu->vm->arch.vgicr_addr;
            size_t acc_offset = GICR_REG_MASK(base_offset);
            if (GICR_IS_REG(TYPER, acc_offset)) {
                handler_info = &vgicr_typer_info;
            } else if (GICR_IS_REG(IPRIORITYR, acc_offset)) {
                handler_info = &ipriorityr_info;
            } else if (GICR_IS_REG(ID, acc_offset)) {
                handler_info = &vgicr_pidr_info;
            } else {
                handler_info = &razwi_info;
            }
        }
    }

    if (vgic_check_reg_alignment(acc, handler_info)) {
        vcpuid_t vgicr_id = vgicr_get_id(acc);
        struct vcpu* vcpu =
            vgicr_id == cpu()->vcpu->id ? cpu()->vcpu : vm_get_vcpu(cpu()->vcpu->vm, vgicr_id);
        spin_lock(&vcpu->arch.vgic_priv.vgicr.lock);
            handler_info->reg_access(acc, handler_info, true, vgicr_id);
        spin_unlock(&vcpu->arch.vgic_priv.vgicr.lock);
        return true;
    } else {
        console_printk("GICv3: Not aligned redist Emulation in address:0x%x\n",acc->addr);
        return false;
    }
}

/*-------------------- ITS -----------------------*/

void vgits_emul_pidr2_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id) 
{
    if (!acc->write) {
        //vcpu_writereg(cpu()->vcpu, acc->reg,gits->ID[((acc->addr & 0xff) - 0xd0) / 4]);
        vcpu_writereg(cpu()->vcpu, acc->reg,0x3b);
    }
}

void vgits_emul_ctlr_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id) 
{
    if (!acc->write) {
        vcpu_writereg(cpu()->vcpu, acc->reg,gits->CTLR);
    }else{
        gits->CTLR=vcpu_readreg(cpu()->vcpu, acc->reg);
    }
}

void vgits_emul_typer_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id) 
{
    if (!acc->write) {
        //TODO: Get the value of PTA to now the RDbase format

        vcpu_writereg(cpu()->vcpu, acc->reg,gits->TYPER);
    }
}

void vgits_emul_cbaser_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id) 
{
    if (!acc->write) {
        vcpu_writereg(cpu()->vcpu, acc->reg,cpu()->vcpu->vm->arch.vgits.CBASER);
    }else{

        struct vm* vm =  cpu()->vcpu->vm;
        uint64_t tmp_cbaser = vcpu_readreg(cpu()->vcpu, acc->reg);
        paddr_t cmdq_pa=0;
        vaddr_t *cbaser_vaddr = (vaddr_t *)(tmp_cbaser & GITS_CBASER_PHY_ADDR_MSK);
        size_t pages = (tmp_cbaser & GITS_CBASER_SIZE_MSK) + 1;

        mem_guest_ipa_translate(cbaser_vaddr,&cmdq_pa);

        //Unmap from the Bao space
        if(vm->arch.vgits.vgits_cmdq.base_vaddr != NULL) {
            mem_unmap(&cpu()->as,(vaddr_t)vm->arch.vgits.vgits_cmdq.base_vaddr,vm->arch.vgits.vgits_cmdq.page_size,true);
        }

        vm->arch.vgits.vgits_cmdq.base_vaddr = (void*)mem_alloc_map_dev(&cpu()->as, SEC_HYP_GLOBAL, INVALID_VA,
        (vaddr_t)cmdq_pa,pages);

        if(vm->arch.vgits.vgits_cmdq.base_vaddr == NULL)
            ERROR("[BAO] VM Command queue not mapped to Bao\n");

        vm->arch.vgits.vgits_cmdq.page_size = pages;
        vm->arch.vgits.CBASER = tmp_cbaser; //aren't there anu hardcoded info?
    }
}





void its_clear_cmd(struct its_cmd* curr_cmd)
{
    for(int cmd_i = 0; cmd_i < 4; cmd_i++)
        curr_cmd->cmd[cmd_i] = 0;
}

void its_build_mapc(struct its_cmd *curr_cmd,
                    struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_MAPC_CMD);
    its_encode_target(curr_cmd,desc->its_mapc_cmd.target);
    its_encode_ic_id(curr_cmd,desc->its_mapc_cmd.ic_id);
    its_encode_valid(curr_cmd,desc->its_mapc_cmd.valid);
}

void its_build_sync(struct its_cmd *curr_cmd,
                    struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_SYNC_CMD);
    its_encode_target(curr_cmd,desc->its_sync_cmd.target);
}

void its_build_mapd(struct its_cmd *curr_cmd,
                    struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_MAPD_CMD);
    its_encode_device_id(curr_cmd,desc->its_mapd_cmd.device_id);
    its_encode_size(curr_cmd,desc->its_mapd_cmd.size);
    its_encode_itt_addr(curr_cmd,desc->its_mapd_cmd.itt_addr);
    its_encode_valid(curr_cmd,desc->its_mapd_cmd.valid);
}

void its_build_inv(struct its_cmd *curr_cmd,
                    struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_INV_CMD);
    its_encode_device_id(curr_cmd,desc->its_inv_cmd.device_id);
    its_encode_event_id(curr_cmd,desc->its_inv_cmd.event_id);
}

void its_build_mapti(struct its_cmd *curr_cmd,
                    struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_MAPTI_CMD);
    its_encode_device_id(curr_cmd,desc->its_mapti_cmd.device_id);
    its_encode_event_id(curr_cmd,desc->its_mapti_cmd.event_id);
    its_encode_pint_id(curr_cmd,desc->its_mapti_cmd.pint_id);
    its_encode_ic_id(curr_cmd,desc->its_mapti_cmd.ic_id);
}

void its_build_mapi(struct its_cmd *curr_cmd,
                    struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_MAPI_CMD);
    its_encode_device_id(curr_cmd,desc->its_mapi_cmd.device_id);
    its_encode_event_id(curr_cmd,desc->its_mapi_cmd.event_id);
    its_encode_ic_id(curr_cmd,desc->its_mapi_cmd.ic_id);
}

void its_build_invall(struct its_cmd *curr_cmd,
                    struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_INVALL_CMD);
    its_encode_ic_id(curr_cmd,desc->its_invall_cmd.ic_id);
}


/* Build Virtual commands for GICv4*/

void its_build_vmapp(struct its_cmd *curr_cmd,
                    struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_VMAPP_CMD);
    its_encode_vpe_id(curr_cmd,desc->its_vmapp_cmd.vpe_id); 
    its_encode_valid(curr_cmd,desc->its_vmapp_cmd.valid);
    its_encode_target(curr_cmd,desc->its_vmapp_cmd.target);
    its_encode_vpt_addr(curr_cmd,desc->its_vmapp_cmd.vpt_addr);
    its_encode_vpt_size(curr_cmd,16);
}

void its_build_vsync(struct its_cmd* curr_cmd, 
                struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_VSYNC_CMD);
    its_encode_vpe_id(curr_cmd,desc->its_vsync_cmd.vpe_id);
}

void its_build_vinvall(struct its_cmd* curr_cmd, 
                struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_VINVALL_CMD);
    its_encode_vpe_id(curr_cmd,desc->its_vinvall_cmd.vpe_id);
}

void its_build_vmapti(struct its_cmd* curr_cmd, 
                struct its_cmd_desc *desc)
{
    its_clear_cmd(curr_cmd);
    its_encode_cmd(curr_cmd,ITS_VMAPTI_CMD);
    its_encode_vpe_id(curr_cmd,desc->its_vmapti_cmd.vpe_id);
    its_encode_device_id(curr_cmd,desc->its_vmapti_cmd.device_id);
    its_encode_event_id(curr_cmd,desc->its_vmapti_cmd.event_id);
    // if()
    //     its_encode_db_id();
    // else
    its_encode_db_id(curr_cmd,desc->its_vmapti_cmd.db_id);

    its_encode_virt_id(curr_cmd,desc->its_vmapti_cmd.virt_id);
}

void its_copy_to_cmdq(struct its_cmd *dest_cmd,
                    struct its_cmd *src_cmd)
{
    for(int i = 0; i < 4; i++)
        dest_cmd->cmd[i] = src_cmd->cmd[i];
}

void its_translate_cmd(struct its_cmd *dest_cmd,
                    struct its_cmd *src_cmd)
{
    vcpuid_t vrdbase;
    cpuid_t pgicr_id, vic_id;

    //TODO LOCK mechanism
    struct its_cmd_desc desc;
    struct vm* vm =  cpu()->vcpu->vm;

    switch (GITS_CMD_MASK(src_cmd)) {
    case ITS_MAPC_CMD:
        
        vrdbase = bit64_extract(src_cmd->cmd[2],ITS_CMD_RDBASE_OFF,ITS_CMD_RDBASE_LEN);
        vic_id = bit64_extract(src_cmd->cmd[2],0,12);  //To-do: See implication of size

        pgicr_id = vm_translate_to_pcpuid(cpu()->vcpu->vm, vrdbase); //To-do: ERROR verification
        vm->arch.vgits.vicid_to_icid_map[vic_id] = pgicr_id;    // associate the vCollID to the physical one

        desc.its_mapc_cmd.target = pgicr_id;
        desc.its_mapc_cmd.ic_id = pgicr_id; //In the physcal cmdqueue the collid is equal to the redistid
        desc.its_mapc_cmd.valid = !!bit64_extract(src_cmd->cmd[2],63,1);

        its_build_mapc(dest_cmd,&desc);

        break;
    case ITS_SYNC_CMD:

        vrdbase = bit64_extract(src_cmd->cmd[2],ITS_CMD_RDBASE_OFF,ITS_CMD_RDBASE_LEN);
        pgicr_id = vm_translate_to_pcpuid(cpu()->vcpu->vm, vrdbase); //To-do: ERROR verification

        desc.its_sync_cmd.target = pgicr_id;

        its_build_sync(dest_cmd,&desc);

        break;
    case ITS_MAPD_CMD:

        struct ppages itt_pages = { .num_pages = 0 };
        itt_pages = mem_alloc_ppages(cpu()->as.colors,16,true); //To-do: Not to safe? Malicious VM can leak the physical mem

        desc.its_mapd_cmd.device_id = bit64_extract(src_cmd->cmd[0],32,32); //for now no control
        desc.its_mapd_cmd.size = 15;
        desc.its_mapd_cmd.itt_addr = itt_pages.base;
        desc.its_mapd_cmd.valid = !!bit64_extract(src_cmd->cmd[2],63,1);

        its_build_mapd(dest_cmd,&desc);

        break;
    case ITS_MAPTI_CMD:

        vic_id = bit64_extract(src_cmd->cmd[2],0,12);  //To-do: See implication of size

        desc.its_mapti_cmd.device_id = bit64_extract(src_cmd->cmd[0],32,32);
        desc.its_mapti_cmd.event_id = bit64_extract(src_cmd->cmd[1],0,32);
        //To-do: verify if id belongs to VM
        desc.its_mapti_cmd.pint_id = bit64_extract(src_cmd->cmd[1],32,32);
        //To-do: error verification
        desc.its_mapti_cmd.ic_id = vm->arch.vgits.vicid_to_icid_map[vic_id];

        its_build_mapti(dest_cmd,&desc);

        break;
    case ITS_MAPI_CMD:

        vic_id = bit64_extract(src_cmd->cmd[2],0,12);  //To-do: See implication of size

        desc.its_mapi_cmd.device_id = bit64_extract(src_cmd->cmd[0],32,32);
        desc.its_mapi_cmd.event_id = bit64_extract(src_cmd->cmd[1],0,32);
        //To-do: error verification
        desc.its_mapi_cmd.ic_id = vm->arch.vgits.vicid_to_icid_map[vic_id];

        its_build_mapi(dest_cmd,&desc);

        break;
    case ITS_INVALL_CMD:

        vic_id = bit64_extract(src_cmd->cmd[2],0,12);  //To-do: See implication of size                                                                                                                     
        desc.its_invall_cmd.ic_id = vm->arch.vgits.vicid_to_icid_map[vic_id];

        its_build_invall(dest_cmd,&desc);

        break;
    case ITS_CLEAR_CMD:
    case ITS_DISCARD_CMD:
    case ITS_INT_CMD:
    case ITS_INV_CMD:
        cache_flush_range((vaddr_t)cpu()->vcpu->vm->arch.prop_tab.proptab_base,0x1000);
        its_copy_to_cmdq(dest_cmd,src_cmd);
    break;
    default: //To-do: further evaluation - stop the hypervisor execution probably not the best solution
        ERROR("[BAO-VGICV3] Invalid ITS command received with id 0x%x\n",GITS_CMD_MASK(src_cmd));
    }
}

void its_translate_vcmd(struct its_cmd *dest_cmd,
                    struct its_cmd *src_cmd)
{
    vcpuid_t vrdbase;
    cpuid_t pgicr_id;
    struct its_cmd_desc desc;

    switch (GITS_CMD_MASK(src_cmd)) {
        case ITS_MAPC_CMD:

            paddr_t pa_vpt;
            struct vcpu *target_vcpu;

            vrdbase = bit64_extract(src_cmd->cmd[2],ITS_CMD_RDBASE_OFF,ITS_CMD_RDBASE_LEN);
            pgicr_id = vm_translate_to_pcpuid(cpu()->vcpu->vm, vrdbase); //ERROR verification
            target_vcpu = vm_get_vcpu(cpu()->vcpu->vm, vrdbase);
            mem_guest_ipa_translate((vaddr_t*)(target_vcpu->arch.vgic_priv.vgicr.PENDBASER & GICR_PENDBASER_PHY_ADDR_MSK),&pa_vpt);

            //with some VM requires other logic
            desc.its_vmapp_cmd.vpe_id = bit64_extract(src_cmd->cmd[2],0,16);   //See imple defined sizes
            desc.its_vmapp_cmd.target = pgicr_id;
            desc.its_vmapp_cmd.vpt_addr = pa_vpt;   //review
            desc.its_vmapp_cmd.vpt_size = bit64_extract(src_cmd->cmd[3],0,5);
            desc.its_vmapp_cmd.valid = !!bit64_extract(src_cmd->cmd[2],63,1);
            
            its_build_vmapp(dest_cmd,&desc);

            break;
        case ITS_INVALL_CMD:
            // TODO - vpe_id receives the same value as the collection
            desc.its_vinvall_cmd.vpe_id = bit64_extract(src_cmd->cmd[2],0,16); //store the vpe info
            its_build_vinvall(dest_cmd,&desc);
            break;
        case ITS_MAPTI_CMD:
            desc.its_vmapti_cmd.device_id = bit64_extract(src_cmd->cmd[0],32,32);
            desc.its_vmapti_cmd.event_id = bit64_extract(src_cmd->cmd[1],0,32);
            desc.its_vmapti_cmd.virt_id = bit64_extract(src_cmd->cmd[1],32,32);
            desc.its_vmapti_cmd.vpe_id = bit64_extract(src_cmd->cmd[2],0,16); //store the vpe info
            desc.its_vmapti_cmd.db_id = desc.its_vmapti_cmd.virt_id;

            its_build_vmapti(dest_cmd,&desc);
            break;
        case ITS_SYNC_CMD:
            desc.its_vsync_cmd.vpe_id = 0; //store the vpe info
            its_build_vsync(dest_cmd,&desc);
            break;
        case ITS_INV_CMD:
            desc.its_inv_cmd.device_id = bit64_extract(src_cmd->cmd[0],32,32);
            desc.its_inv_cmd.event_id = bit64_extract(src_cmd->cmd[1],0,32);

            its_build_inv(dest_cmd,&desc);
            break;
        case ITS_MAPD_CMD:
            paddr_t itt_paddr;
            vaddr_t *itt_vaddr = (vaddr_t *)bit64_extract(src_cmd->cmd[2],0,52);
            mem_guest_ipa_translate(itt_vaddr,&itt_paddr);

            desc.its_mapd_cmd.device_id = bit64_extract(src_cmd->cmd[0],32,32);
            desc.its_mapd_cmd.size = bit64_extract(src_cmd->cmd[1],0,5);
            desc.its_mapd_cmd.itt_addr = itt_paddr;
            desc.its_mapd_cmd.valid = !!bit64_extract(src_cmd->cmd[2],63,1);

            its_build_mapd(dest_cmd,&desc);
            break;
        default:
            //TODO: introduce a "NOP" command
       
    }
}

void vgits_emul_cwriter_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id)
{
    uint64_t prev_cwriter, curr_cwriter, cmd_off,vm_cmd_off;
    size_t n_cmd, num_cmd;
    struct its_cmd *vm_cmd, *its_cmd;


    struct vm* vm =  cpu()->vcpu->vm;
    //uint64_t *its_test;


    if (!acc->write) {
        vcpu_writereg(cpu()->vcpu, acc->reg,vm->arch.vgits.CWRITER);
    }else{

        //Number of commands to translate
        prev_cwriter = vm->arch.vgits.CWRITER;
        vm_cmd_off =  prev_cwriter/0x20;
        cmd_off = gits->CWRITER/0x20;
        curr_cwriter = vcpu_readreg(cpu()->vcpu, acc->reg);
        n_cmd = (prev_cwriter > curr_cwriter)? (((4096 * (ITS_CMD_QUEUE_N_PAGE + 1))- prev_cwriter) + curr_cwriter)/0x20 : (curr_cwriter - prev_cwriter)/0x20;
        num_cmd = n_cmd;

        vm_cmd = vm->arch.vgits.vgits_cmdq.base_vaddr + vm_cmd_off;
        its_cmd = its_cmd_queue + cmd_off;

        cache_flush_range((vaddr_t)vm->arch.vgits.vgits_cmdq.base_vaddr,0x10000);   //To-do: use vm size
        
        while(n_cmd-- > 0)
        {
            #if (GIC_VERSION == GICV4)
                its_translate_vcmd(its_cmd,vm_cmd);
            #else
                its_translate_cmd(its_cmd,vm_cmd);
            #endif
            vm_cmd++;
            its_cmd++;
        }

        cache_flush_range((vaddr_t)its_cmd_queue,0x10000);

        
        vm->arch.vgits.CWRITER = curr_cwriter;
        gits->CWRITER += (num_cmd * 0x20); 
    }
}

void vgits_emul_creadr_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id) 
{
    if (!acc->write) {  //read only
        vcpu_writereg(cpu()->vcpu, acc->reg,cpu()->vcpu->vm->arch.vgits.CWRITER);
    }
}

void vgits_emul_baser_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id) 
{
    size_t index = (GITS_REG_MASK(acc->addr) - GITS_REG_OFF(BASER)) >> 3;

    if (!acc->write) {
        vcpu_writereg(cpu()->vcpu, acc->reg,(cpu()->vcpu->vm->arch.vgits.BASER[index]));
    }else{
        uint64_t tmp = vcpu_readreg(cpu()->vcpu, acc->reg);
        cpu()->vcpu->vm->arch.vgits.BASER[index] = (cpu()->vcpu->vm->arch.vgits.BASER[index] & GITS_BASER_RO_MASK) | (tmp & ~GITS_BASER_RO_MASK);
    }
}

void vgits_emul_iidr_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id) 
{
    if (!acc->write) { //read only
        vcpu_writereg(cpu()->vcpu, acc->reg,gits->IIDR);  //Only gicv3 is visible for the guest
    }
}

void vgits_emul_tkr_access(struct emul_access* acc, struct vgic_reg_handler_info* handlers,
    bool gicr_access, vcpuid_t vgicr_id)
{    
    size_t acc_offset = GITS_REG_MASK(acc->addr);
    uint64_t gits_base = (uint64_t)gits;
    uint32_t *ptr = (uint32_t*)(gits_base+acc_offset);

    if (!acc->write) { //read only
        vcpu_writereg(cpu()->vcpu, acc->reg,*ptr);  //Only gicv3 is visible for the guest
    } else {
        *ptr = vcpu_readreg(cpu()->vcpu, acc->reg);
    }
}

struct vgic_reg_handler_info vgits_pidr2_info = {
    vgits_emul_pidr2_access,
    0b0100,
};

struct vgic_reg_handler_info vgits_ctlr_access = {
    vgits_emul_ctlr_access,
    0b0100,
};

struct vgic_reg_handler_info vgits_typer_access = {
    vgits_emul_typer_access,
    0b1000,
};
struct vgic_reg_handler_info vgits_cbaser_access = {
    vgits_emul_cbaser_access,
    0b1000,
};

struct vgic_reg_handler_info vgits_cwriter_access = {
    vgits_emul_cwriter_access,
    0b1100,
};

struct vgic_reg_handler_info vgits_baser_access = {
    vgits_emul_baser_access,
    0b1000,
};
/*struct vgic_reg_handler_info vgits_translater_access = {
    vgits_emul_translater_access,
    0b0100,
};*/


struct vgic_reg_handler_info vgits_creadr_access = {
    vgits_emul_creadr_access,
    0b1100,
};

struct vgic_reg_handler_info vgits_iidr_access = {
    vgits_emul_iidr_access,
    0b0100,
};

struct vgic_reg_handler_info vgits_tkr_access = {
    vgits_emul_tkr_access,
    0b0100,
};


/* ITS emul handler */
bool vgits_emul_handler(struct emul_access* acc){

    struct vgic_reg_handler_info* handler_info = NULL;
    struct vm *vm = cpu()->vcpu->vm;

     switch (GITS_REG_MASK(acc->addr)) {
        case GITS_REG_OFF(CTLR):
            handler_info = &vgits_ctlr_access;
            break;
        case GITS_REG_OFF(IIDR):
            handler_info = &vgits_iidr_access;
            break;
        case GITS_REG_OFF(TYPER):
            handler_info = &vgits_typer_access;
        break;
        case GITS_REG_OFF(CBASER):
            handler_info = &vgits_cbaser_access;
        break;
        case GITS_REG_OFF(CWRITER):
            handler_info = &vgits_cwriter_access;
            break;
        case GITS_REG_OFF(CREADR):
            handler_info = &vgits_creadr_access;
            break;
        /*case GITS_REG_OFF(TRANSLATER):
            handler_info = &vgits_translater_access;
            break;*/
        default: {
            size_t base_offset = acc->addr - cpu()->vcpu->vm->arch.vgicr_addr;
            size_t acc_offset = GITS_REG_MASK(base_offset);
            if (GITS_IS_REG(BASER, acc_offset)) {
                handler_info = &vgits_baser_access;
            }  else if (GITS_IS_REG(ID, acc_offset)) {
                handler_info = &vgits_pidr2_info;
            } else if (acc->addr == 0x51a2c000 || acc->addr == 0x51a2c004 || acc->addr == 0x51a2c008 ||acc->addr == 0x51a2c00c){
                handler_info = &vgits_tkr_access;
            } else {
                handler_info = &razwi_info;
            }             
        }
    }

    if (vgic_check_reg_alignment(acc, handler_info)) {
        spin_lock(&vm->arch.vgits.lock);
            handler_info->reg_access(acc, handler_info, false, 0);
        spin_unlock(&vm->arch.vgits.lock);

        return true;
    } else {
        return false;
    }
}

bool vgic_icc_sgir_handler(struct emul_access* acc)
{
    if (acc->write) {
        uint64_t sgir = vcpu_readreg(cpu()->vcpu, acc->reg);
        if (acc->multi_reg) {
            uint64_t sgir_high = vcpu_readreg(cpu()->vcpu, acc->reg_high);
            sgir |= (sgir_high << 32);
        }
        irqid_t int_id = ICC_SGIR_SGIINTID(sgir);
        cpumap_t trgtlist;
        if (sgir & ICC_SGIR_IRM_BIT) {
            trgtlist = cpu()->vcpu->vm->cpus & ~(1U << cpu()->vcpu->phys_id);
        } else {
            /**
             * TODO: we are assuming the vm has a single cluster. Change this when adding virtual
             * cluster support.
             */
            trgtlist = vm_translate_to_pcpu_mask(cpu()->vcpu->vm, ICC_SGIR_TRGLSTFLT(sgir),
                cpu()->vcpu->vm->cpu_num);
        }
        vgic_send_sgi_msg(cpu()->vcpu, trgtlist, int_id);
    }

    return true;
}

bool vgic_icc_sre_handler(struct emul_access* acc)
{
    if (!acc->write) {
        vcpu_writereg(cpu()->vcpu, acc->reg, 0x1);
    }
    return true;
}

void vgic_init(struct vm* vm, const struct vgic_dscrp* vgic_dscrp)
{
    vm->arch.vgicr_addr = vgic_dscrp->gicr_addr;
    vm->arch.vgicd.CTLR = 0;
    vm->msi = (vgic_dscrp->gits_addr ? true : false);
    size_t vtyper_itln = vgic_get_itln(vgic_dscrp);
    vm->arch.vgicd.int_num = 32 * (vtyper_itln + 1);
    vm->arch.vgicd.TYPER = ((vtyper_itln << GICD_TYPER_ITLN_OFF) & GICD_TYPER_ITLN_MSK) |
        (((vm->cpu_num - 1) << GICD_TYPER_CPUNUM_OFF) & GICD_TYPER_CPUNUM_MSK) |
        ((((vm->msi ? 16 : 10) - 1) << GICD_TYPER_IDBITS_OFF) & GICD_TYPER_IDBITS_MSK) |
        (vm->msi? GICD_TYPER_LPIS_BIT : 0); //LPI support
    vm->arch.vgicd.IIDR = gicd->IIDR;

    size_t vgic_int_size = vm->arch.vgicd.int_num * sizeof(struct vgic_int);
    vm->arch.vgicd.interrupts = mem_alloc_page(NUM_PAGES(vgic_int_size), SEC_HYP_VM, false);
    if (vm->arch.vgicd.interrupts == NULL) {
        ERROR("failed to alloc vgic");
    }

    for (size_t i = 0; i < vm->arch.vgicd.int_num; i++) {
        vm->arch.vgicd.interrupts[i].owner = NULL;
        vm->arch.vgicd.interrupts[i].lock = SPINLOCK_INITVAL;
        vm->arch.vgicd.interrupts[i].id = i + GIC_CPU_PRIV;
        vm->arch.vgicd.interrupts[i].state = INV;
        vm->arch.vgicd.interrupts[i].prio = GIC_LOWEST_PRIO;
        vm->arch.vgicd.interrupts[i].cfg = 0;
        vm->arch.vgicd.interrupts[i].route = GICD_IROUTER_INV;
        vm->arch.vgicd.interrupts[i].phys.route = GICD_IROUTER_INV;
        vm->arch.vgicd.interrupts[i].hw = false;
        vm->arch.vgicd.interrupts[i].in_lr = false;
        vm->arch.vgicd.interrupts[i].enabled = false;
    }

    vm->arch.vgicd_emul = (struct emul_mem){ .va_base = vgic_dscrp->gicd_addr,
        .size = ALIGN(sizeof(struct gicd_hw), PAGE_SIZE),
        .handler = vgicd_emul_handler };
    vm_emul_add_mem(vm, &vm->arch.vgicd_emul);

    /* Initialize virtual its registers*/
    for (vcpuid_t vcpuid = 0; vcpuid < vm->cpu_num; vcpuid++) {
        struct vcpu* vcpu = vm_get_vcpu(vm, vcpuid);
        uint64_t typer = (uint64_t)vcpu->id << GICR_TYPER_PRCNUM_OFF;
        typer |= ((uint64_t)vcpu->arch.vmpidr & MPIDR_AFF_MSK) << GICR_TYPER_AFFVAL_OFF;
        typer |= !!(vcpu->id == vcpu->vm->cpu_num - 1) << GICR_TYPER_LAST_OFF;
        typer |= (vm->msi ? 0x1 : 0x0);   /*enable PLPIS*/
        vcpu->arch.vgic_priv.vgicr.TYPER = typer;
        vcpu->arch.vgic_priv.vgicr.IIDR = gicr[cpu()->id].IIDR;
    }
    /*GIC version of cpu interface*/

    vm->arch.vgicr_emul = (struct emul_mem){ .va_base = vgic_dscrp->gicr_addr,
        .size = ALIGN(sizeof(struct gicr_hw) - GICR_VFRAME_SIZE, PAGE_SIZE) * vm->cpu_num,
        .handler = vgicr_emul_handler };
    vm_emul_add_mem(vm, &vm->arch.vgicr_emul);

    /*ITS/LPI emulation process*/
    if(vm->msi){
        for (size_t index = 0; index < GIC_MAX_TTD; index++) {
            //TODO -  Verify if flat tables are supported and manage Indirect bit
            if(bit64_extract(gits->BASER[index], GITS_BASER_TYPE_OFF, GITS_BASER_TYPE_LEN) == GITS_BASER_VPET_TYPE)
            {
                vm->arch.vgits.BASER[index]= 0;
            } else {
                vm->arch.vgits.BASER[index]= (gits->BASER[index] & GITS_BASER_RO_MASK);
            }
        }

        /*CBASER*/

        /*TYPER*/
        //TODO Create pidr2 virtual registers to each VM
        vm->arch.vgits.TYPER = gits->TYPER & ~GITS_TYPER_VIRT_MSK;

        /*CWRITER*/
        vm->arch.vgits.CWRITER = 0;

        /*CTLR */


        /*CollID map init*/
        for (size_t index = 0; index < GITS_MAX_VCID; index++)
            vm->arch.vgits.vicid_to_icid_map[index] = -1;


        // uint64_t gits_typer = 1ULL << GITS_TYPER_PHY_OFF;
        // gits_typer |= 



        vm->arch.vgits_emul = (struct emul_mem){ .va_base = vgic_dscrp->gits_addr,
            .size = ALIGN(sizeof(struct gits_hw), PAGE_SIZE),
            .handler = vgits_emul_handler };
        vm_emul_add_mem(vm, &vm->arch.vgits_emul);
    }


    vm->arch.icc_sgir_emul = (struct emul_reg){ .addr = SYSREG_ENC_ADDR(3, 0, 12, 11, 5),
        .handler = vgic_icc_sgir_handler };
    vm_emul_add_reg(vm, &vm->arch.icc_sgir_emul);

    vm->arch.icc_sre_emul = (struct emul_reg){ .addr = SYSREG_ENC_ADDR(3, 0, 12, 12, 5),
        .handler = vgic_icc_sre_handler };
    vm_emul_add_reg(vm, &vm->arch.icc_sre_emul);

    list_init(&vm->arch.vgic_spilled);
    vm->arch.vgic_spilled_lock = SPINLOCK_INITVAL;
}

void vgic_cpu_init(struct vcpu* vcpu)
{
    for (size_t i = 0; i < GIC_CPU_PRIV; i++) {
        vcpu->arch.vgic_priv.interrupts[i].owner = NULL;
        vcpu->arch.vgic_priv.interrupts[i].lock = SPINLOCK_INITVAL;
        vcpu->arch.vgic_priv.interrupts[i].id = i;
        vcpu->arch.vgic_priv.interrupts[i].state = INV;
        vcpu->arch.vgic_priv.interrupts[i].prio = GIC_LOWEST_PRIO;
        vcpu->arch.vgic_priv.interrupts[i].cfg = 0;
        vcpu->arch.vgic_priv.interrupts[i].route = GICD_IROUTER_INV;
        vcpu->arch.vgic_priv.interrupts[i].phys.redist = vcpu->phys_id;
        vcpu->arch.vgic_priv.interrupts[i].hw = false;
        vcpu->arch.vgic_priv.interrupts[i].in_lr = false;
        vcpu->arch.vgic_priv.interrupts[i].enabled = false;
    }

    for (size_t i = 0; i < GIC_MAX_SGIS; i++) {
        vcpu->arch.vgic_priv.interrupts[i].cfg = 0b10;
    }

    list_init(&vcpu->arch.vgic_spilled);
}

static inline uint8_t vgic_get_prio_lpi(struct vm *vm, irqid_t id){
    return vm->arch.prop_tab.proptab_base[id - GIC_FIRST_LPIS] & LPI_CONFIG_PRIO_MSK;
}

static inline uint8_t vgic_get_en_lpi(struct vm *vm, irqid_t id){
    return vm->arch.prop_tab.proptab_base[id - GIC_FIRST_LPIS] & LPI_CONFIG_EN_MSK;
}

struct vgic_int vgic_tmp_lpi(struct vcpu* vcpu, irqid_t id){
    struct vgic_int interrupt;

    interrupt.lock = SPINLOCK_INITVAL;
    interrupt.owner = vcpu;
    interrupt.state = PEND;
    interrupt.in_lr = false;
    interrupt.id = id;
    interrupt.prio = vgic_get_prio_lpi(vcpu->vm,id);
    interrupt.cfg = 0;
    interrupt.phys.redist = vcpu->phys_id;
    interrupt.hw = false;
    interrupt.enabled = vgic_get_en_lpi(vcpu->vm,id);

    return interrupt;
}

void vgic_inject_msi(struct vcpu* vcpu, irqid_t id){

    struct vgic_int tmp_interrupt = vgic_tmp_lpi(vcpu,id);

    vgic_add_lr(vcpu,&tmp_interrupt);
}
