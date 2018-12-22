/*
  NoirVisor - Hardware-Accelerated Hypervisor solution

  Copyright 2018, Zero Tang. All rights reserved.

  This file is the basic driver of Intel VT-x.

  This program is distributed in the hope that it will be useful, but 
  without any warranty (no matter implied warranty or merchantability
  or fitness for a particular purpose, etc.).

  File Location: /vt_core/vt_main.c
*/

#include <nvdef.h>
#include <nvstatus.h>
#include <nvbdk.h>
#include <vt_intrin.h>
#include <noirhvm.h>
#include <intrin.h>
#include <ia32.h>
#include "vt_vmcs.h"
#include "vt_def.h"

bool nvc_is_vt_supported()
{
	u32 c;
	noir_cpuid(1,0,null,null,&c,null);
	if(noir_bt(&c,ia32_cpuid_vmx))
	{
		bool basic_supported=true;
		ia32_vmx_basic_msr vt_basic;
		vt_basic.value=noir_rdmsr(ia32_vmx_basic);
		if(vt_basic.memory_type==6)		//Make sure CPU supports Write-Back VMCS.
		{
			ia32_vmx_priproc_ctrl_msr prictrl_msr;
			if(vt_basic.use_true_msr)
				prictrl_msr.value=noir_rdmsr(ia32_vmx_true_priproc_ctrl);
			else
				prictrl_msr.value=noir_rdmsr(ia32_vmx_priproc_ctrl);
			//Support of MSR-Bitmap is essential.
			basic_supported&=prictrl_msr.allowed1_settings.use_msr_bitmap;
		}
		return basic_supported;
	}
	return false;
}

void static nvc_vt_cleanup(noir_hypervisor_p hvm)
{
	if(hvm)
	{
		noir_vt_hvm_p rhvm=hvm->relative_hvm;
		if(hvm->virtual_cpu)
		{
			u32 i=0;
			noir_vt_vcpu_p vcpu=hvm->virtual_cpu;
			for(;i<hvm->cpu_count;vcpu=&hvm->virtual_cpu[++i])
			{
				if(vcpu->vmxon.virt)
					noir_free_contd_memory(vcpu->vmxon.virt);
				if(vcpu->vmcs.virt)
					noir_free_contd_memory(vcpu->vmcs.virt);
				if(vcpu->hv_stack)
					noir_free_nonpg_memory(vcpu->hv_stack);
			}
			noir_free_nonpg_memory(hvm->virtual_cpu);
		}
		if(rhvm->msr_bitmap.virt)
			noir_free_contd_memory(rhvm->msr_bitmap.virt);
		if(rhvm->io_bitmap_a.virt)
			noir_free_contd_memory(rhvm->io_bitmap_a.virt);
		if(rhvm->io_bitmap_b.virt)
			noir_free_contd_memory(rhvm->io_bitmap_b.virt);
		if(rhvm->msr_auto_list.virt)
			noir_free_contd_memory(rhvm->msr_auto_list.virt);
	}
}

void static nvc_vt_setup_msr_auto_list(noir_hypervisor_p hvm)
{
	/*
		For MSR-Auto list, we have following convention:
		MSR-Auto list is a contiguous 4KB memory region.
		1st 1KB is for MSR-load at VM-Entry.
		2nd 1KB is for MSR-load at VM-Exit.
		3rd 1KB is for MSR-store at VM-Exit.
		4th 1KB is reserved and MBZ.
		Note that this is not defined by Intel Architecture.
	*/
	ia32_vmx_msr_auto_p entry_load=(ia32_vmx_msr_auto_p)((ulong_ptr)hvm->relative_hvm->msr_auto_list.virt+0);
	ia32_vmx_msr_auto_p exit_load=(ia32_vmx_msr_auto_p)((ulong_ptr)hvm->relative_hvm->msr_auto_list.virt+0x400);
	ia32_vmx_msr_auto_p exit_store=(ia32_vmx_msr_auto_p)((ulong_ptr)hvm->relative_hvm->msr_auto_list.virt+0x800);
	//Setup custom MSR-Auto list.
#if defined(_amd64)
	entry_load[0].index=ia32_lstar;
	entry_load[0].data=(ulong_ptr)noir_system_call;
	exit_load[0].index=ia32_lstar;
	exit_load[0].data=orig_system_call;
#endif
}

void static nvc_vt_setup_msr_hook(noir_hypervisor_p hvm)
{
	void* read_bitmap_low=(void*)((ulong_ptr)hvm->relative_hvm->msr_bitmap.virt+0);
	void* read_bitmap_high=(void*)((ulong_ptr)hvm->relative_hvm->msr_bitmap.virt+0x400);
	void* write_bitmap_low=(void*)((ulong_ptr)hvm->relative_hvm->msr_bitmap.virt+0x800);
	void* write_bitmap_high=(void*)((ulong_ptr)hvm->relative_hvm->msr_bitmap.virt+0xC00);
	//Setup custom MSR-Interception.
#if defined(_amd64)
	noir_set_bitmap(read_bitmap_high,ia32_lstar-0xC0000000);	//Hide MSR Hook
#else
	noir_set_bitmap(read_bitmap_low,ia32_sysenter_eip);			//Hide MSR Hook
#endif
}

u8 static nvc_vt_enable(u64* vmxon_phys)
{
	u64 cr0=noir_readcr0();
	u64 cr4=noir_readcr4();
	cr0&=noir_rdmsr(ia32_vmx_cr0_fixed0);
	cr0|=noir_rdmsr(ia32_vmx_cr0_fixed1);
	cr4&=noir_rdmsr(ia32_vmx_cr4_fixed0);
	cr4|=noir_rdmsr(ia32_vmx_cr4_fixed1);
	noir_writecr0(cr0);
	noir_writecr4(cr4);
	return noir_vt_vmxon(vmxon_phys);
}

void static nvc_vt_setup_guest_state_area(noir_processor_state_p state_p)
{
	//Guest State Area - CS Segment
	noir_vt_vmwrite(guest_cs_selector,state_p->cs.selector);
	noir_vt_vmwrite(guest_cs_limit,state_p->cs.limit);
	noir_vt_vmwrite(guest_cs_access_rights,vt_attrib(state_p->cs.selector,state_p->cs.attrib));
	noir_vt_vmwrite(guest_cs_base,state_p->cs.base);
	//Guest State Area - DS Segment
	noir_vt_vmwrite(guest_ds_selector,state_p->ds.selector);
	noir_vt_vmwrite(guest_ds_limit,state_p->ds.limit);
	noir_vt_vmwrite(guest_ds_access_rights,vt_attrib(state_p->ds.selector,state_p->ds.attrib));
	noir_vt_vmwrite(guest_ds_base,state_p->ds.base);
	//Guest State Area - ES Segment
	noir_vt_vmwrite(guest_es_selector,state_p->es.selector);
	noir_vt_vmwrite(guest_es_limit,state_p->es.limit);
	noir_vt_vmwrite(guest_es_access_rights,vt_attrib(state_p->es.selector,state_p->es.attrib));
	noir_vt_vmwrite(guest_es_base,state_p->es.base);
	//Guest State Area - FS Segment
	noir_vt_vmwrite(guest_fs_selector,state_p->fs.selector);
	noir_vt_vmwrite(guest_fs_limit,state_p->fs.limit);
	noir_vt_vmwrite(guest_fs_access_rights,vt_attrib(state_p->fs.selector,state_p->fs.attrib));
	noir_vt_vmwrite(guest_fs_base,state_p->fs.base);
	//Guest State Area - GS Segment
	noir_vt_vmwrite(guest_gs_selector,state_p->gs.selector);
	noir_vt_vmwrite(guest_gs_limit,state_p->gs.limit);
	noir_vt_vmwrite(guest_gs_access_rights,vt_attrib(state_p->gs.selector,state_p->gs.attrib));
	noir_vt_vmwrite(guest_gs_base,state_p->gs.base);
	//Guest State Area - SS Segment
	noir_vt_vmwrite(guest_ss_selector,state_p->ss.selector);
	noir_vt_vmwrite(guest_ss_limit,state_p->ss.limit);
	noir_vt_vmwrite(guest_ss_access_rights,vt_attrib(state_p->ss.selector,state_p->ss.attrib));
	noir_vt_vmwrite(guest_ss_base,state_p->ss.base);
	//Guest State Area - Task Register
	noir_vt_vmwrite(guest_tr_selector,state_p->tr.selector);
	noir_vt_vmwrite(guest_tr_limit,state_p->tr.limit);
	noir_vt_vmwrite(guest_tr_access_rights,vt_attrib(state_p->tr.selector,state_p->tr.attrib));
	noir_vt_vmwrite(guest_tr_base,state_p->tr.base);
	//Guest State Area - Local Descriptor Table Register
	noir_vt_vmwrite(guest_ldtr_selector,state_p->ldtr.selector);
	noir_vt_vmwrite(guest_ldtr_limit,state_p->ldtr.limit);
	noir_vt_vmwrite(guest_ldtr_access_rights,vt_attrib(state_p->ldtr.selector,state_p->ldtr.attrib));
	noir_vt_vmwrite(guest_ldtr_base,state_p->ldtr.base);
	//Guest State Area - IDTR and GDTR
	noir_vt_vmwrite(guest_gdtr_base,state_p->gdtr.base);
	noir_vt_vmwrite(guest_idtr_base,state_p->idtr.base);
	noir_vt_vmwrite(guest_gdtr_limit,state_p->gdtr.limit);
	noir_vt_vmwrite(guest_idtr_limit,state_p->idtr.limit);
	//Guest State Area - Control Registers
	noir_vt_vmwrite(guest_cr0,state_p->cr0);
	noir_vt_vmwrite(guest_cr3,state_p->cr3);
	noir_vt_vmwrite(guest_cr4,state_p->cr4);
	//Guest State Area - Debug Controls
	noir_vt_vmwrite(guest_dr7,state_p->dr7);
	//VMCS Link Pointer - Essential for VMX Nesting
	noir_vt_vmwrite(vmcs_link_pointer,0xffffffffffffffff);
}

void static nvc_vt_setup_host_state_area(noir_vt_vcpu_p vcpu,noir_processor_state_p state_p)
{
	//Host State Area - Segment Selectors
	noir_vt_vmwrite(host_cs_selector,state_p->cs.selector);
	noir_vt_vmwrite(host_ds_selector,state_p->ds.selector);
	noir_vt_vmwrite(host_es_selector,state_p->es.selector);
	noir_vt_vmwrite(host_fs_selector,state_p->fs.selector);
	noir_vt_vmwrite(host_gs_selector,state_p->gs.selector);
	noir_vt_vmwrite(host_ss_selector,state_p->ss.selector);
	noir_vt_vmwrite(host_tr_selector,state_p->tr.selector);
	//Host State Area - Control Registers
	noir_vt_vmwrite(host_cr0,state_p->cr0);
	noir_vt_vmwrite(host_cr3,system_cr3);	//We should use the system page table.
	noir_vt_vmwrite(host_cr4,state_p->cr4);
	//Host State Area - Stack Pointer, Instruction Pointer
	noir_vt_vmwrite(host_rsp,(ulong_ptr)vcpu->hv_stack+nvc_stack_size-0x20);
	noir_vt_vmwrite(host_rip,(ulong_ptr)nvc_vt_exit_handler_a);
}

void static nvc_vt_setup_pinbased_controls(bool true_msr)
{
	ia32_vmx_pinbased_controls pin_ctrl;
	//Read MSR to confirm supported capabilities.
	ia32_vmx_pinbased_ctrl_msr pin_ctrl_msr;
	if(true_msr)
		pin_ctrl_msr.value=noir_rdmsr(ia32_vmx_true_pinbased_ctrl);
	else
		pin_ctrl_msr.value=noir_rdmsr(ia32_vmx_pinbased_ctrl);
	//Setup Pin-Based VM-Execution Controls
	pin_ctrl.value=0;
	//Filter unsupported fields.
	pin_ctrl.value|=pin_ctrl_msr.allowed0_settings.value;
	pin_ctrl.value&=pin_ctrl_msr.allowed1_settings.value;
	//Write to VMCS.
	noir_vt_vmwrite(pin_based_vm_execution_controls,pin_ctrl.value);
}

void static nvc_vt_setup_procbased_controls(bool true_msr)
{
	ia32_vmx_priproc_controls proc_ctrl;
	//Read MSR to confirm supported capabilities.
	ia32_vmx_priproc_ctrl_msr proc_ctrl_msr;
	if(true_msr)
		proc_ctrl_msr.value=noir_rdmsr(ia32_vmx_true_priproc_ctrl);
	else
		proc_ctrl_msr.value=noir_rdmsr(ia32_vmx_priproc_ctrl);
	//Setup Primary Processor-Based VM-Execution Controls
	proc_ctrl.value=0;
	proc_ctrl.use_msr_bitmap=1;		//Essential feature for hiding MSR-Hook.
	proc_ctrl.activate_secondary_controls=1;
	//Filter unsupported fields.
	proc_ctrl.value|=proc_ctrl_msr.allowed0_settings.value;
	proc_ctrl.value&=proc_ctrl_msr.allowed1_settings.value;
	if(!proc_ctrl.use_msr_bitmap)
		nv_dprintf("MSR-Hook Hiding is not supported!\n");
	//Write to VMCS.
	noir_vt_vmwrite(primary_processor_based_vm_execution_controls,proc_ctrl.value);
	//Secondary Processor-Based VM-Execution Controls
	if(proc_ctrl.activate_secondary_controls)
	{
		ia32_vmx_2ndproc_controls proc_ctrl2;
		//Read MSR to confirm supported capabilities.
		ia32_vmx_2ndproc_ctrl_msr proc_ctrl2_msr;
		proc_ctrl2_msr.value=noir_rdmsr(ia32_vmx_2ndproc_ctrl);
		//Setup Secondary Processor-Based VM-Execution Controls
		proc_ctrl2.value=0;
		proc_ctrl2.enable_rdtscp=1;
		proc_ctrl2.enable_invpcid=1;
		proc_ctrl2.enable_xsaves_xrstors=1;
		//Filter unsupported fields.
		proc_ctrl2.value|=proc_ctrl2_msr.allowed0_settings.value;
		proc_ctrl2.value&=proc_ctrl2_msr.allowed1_settings.value;
		//Write to VMCS.
		noir_vt_vmwrite(secondary_processor_based_vm_execution_controls,proc_ctrl2.value);
	}
}

void static nvc_vt_setup_vmexit_controls(bool true_msr)
{
	ia32_vmx_exit_controls exit_ctrl;
	//Read MSR to confirm supported capabilities.
	ia32_vmx_exit_ctrl_msr exit_ctrl_msr;
	if(true_msr)
		exit_ctrl_msr.value=noir_rdmsr(ia32_vmx_true_exit_ctrl);
	else
		exit_ctrl_msr.value=noir_rdmsr(ia32_vmx_exit_ctrl);
	//Setup VM-Exit Controls
	exit_ctrl.value=0;
#if defined(_amd64)
	//This field should be set if NoirVisor is in 64-Bit mode.
	exit_ctrl.host_address_space_size=1;
#endif
	//Filter unsupported fields.
	exit_ctrl.value|=exit_ctrl_msr.allowed0_settings.value;
	exit_ctrl.value&=exit_ctrl_msr.allowed1_settings.value;
	//Write to VMCS.
	noir_vt_vmwrite(vmexit_controls,exit_ctrl.value);
}

void static nvc_vt_setup_vmentry_controls(bool true_msr)
{
	ia32_vmx_entry_controls entry_ctrl;
	//Read MSR to confirm supported capabilities.
	ia32_vmx_entry_ctrl_msr entry_ctrl_msr;
	if(true_msr)
		entry_ctrl_msr.value=noir_rdmsr(ia32_vmx_true_entry_ctrl);
	else
		entry_ctrl_msr.value=noir_rdmsr(ia32_vmx_entry_ctrl);
	//Setup VM-Exit Controls
	entry_ctrl.value=0;
#if defined(_amd64)
	//This field should be set if NoirVisor is in 64-Bit mode.
	entry_ctrl.ia32e_mode_guest=1;
#endif
	//Filter unsupported fields.
	entry_ctrl.value|=entry_ctrl_msr.allowed0_settings.value;
	entry_ctrl.value&=entry_ctrl_msr.allowed1_settings.value;
	//Write to VMCS.
	noir_vt_vmwrite(vmentry_controls,entry_ctrl.value);
}

u8 nvc_vt_subvert_processor_i(noir_vt_vcpu_p vcpu,ulong_ptr gsp,ulong_ptr gip)
{
	ia32_vmx_basic_msr vt_basic;
	noir_processor_state state;
	noir_save_processor_state(&state);
	//Issue a sequence of vmwrite instructions to setup VMCS.
	nvc_vt_setup_guest_state_area(&state);
	nvc_vt_setup_host_state_area(vcpu,&state);
	//Setup Control Area
	vt_basic.value=noir_rdmsr(ia32_vmx_basic);
	nvc_vt_setup_pinbased_controls(vt_basic.use_true_msr);
	nvc_vt_setup_procbased_controls(vt_basic.use_true_msr);
	nvc_vt_setup_vmexit_controls(vt_basic.use_true_msr);
	nvc_vt_setup_vmentry_controls(vt_basic.use_true_msr);
	//Guest State Area - Flags, Stack Pointer, Instruction Pointer
	noir_vt_vmwrite(guest_rsp,gsp);
	noir_vt_vmwrite(guest_rip,gip);
	noir_vt_vmwrite(guest_rflags,2);		//That the essential bit is set is fine.
	//Everything are done, perform subversion.
	return noir_vt_vmlaunch();
}

void static nvc_vt_subvert_processor(noir_vt_vcpu_p vcpu)
{
	u8 vst=nvc_vt_enable(&vcpu->vmxon.phys);
	if(vst==vmx_success)
	{
		vst=noir_vt_vmclear(&vcpu->vmcs.phys);
		if(vst==vmx_success)
		{
			vst=noir_vt_vmptrld(&vcpu->vmcs.phys);
			if(vst==vmx_success)
			{
				//At this time, VMCS has been pointed successfully.
				//Start subverting.
				vst=nvc_vt_subvert_processor_a(vcpu);
			}
		}
	}
}

void static nvc_vt_subvert_processor_thunk(void* context,u32 processor_id)
{
	noir_vt_vcpu_p vcpu=(noir_vt_vcpu_p)context;
	ia32_vmx_basic_msr vt_basic;
	vt_basic.value=noir_rdmsr(ia32_vmx_basic);
	*(u32*)vcpu[processor_id].vmxon.virt=(u32)vt_basic.revision_id;
	*(u32*)vcpu[processor_id].vmcs.virt=(u32)vt_basic.revision_id;
	nvc_vt_subvert_processor(&vcpu[processor_id]);
}

noir_status nvc_vt_subvert_system(noir_hypervisor_p hvm)
{
	hvm->cpu_count=noir_get_processor_count();
	hvm->virtual_cpu=noir_alloc_nonpg_memory(hvm->cpu_count*sizeof(noir_vt_vcpu));
	if(hvm->virtual_cpu)
	{
		u32 i=0;
		for(;i<hvm->cpu_count;i++)
		{
			noir_vt_vcpu_p vcpu=&hvm->virtual_cpu[i];
			vcpu->vmcs.virt=noir_alloc_contd_memory(page_size);
			if(vcpu->vmcs.virt)
				vcpu->vmcs.phys=noir_get_physical_address(vcpu->vmcs.virt);
			else
				goto alloc_failure;
			vcpu->vmxon.virt=noir_alloc_contd_memory(page_size);
			if(vcpu->vmxon.virt)
				vcpu->vmxon.phys=noir_get_physical_address(vcpu->vmxon.virt);
			else
				goto alloc_failure;
			vcpu->hv_stack=noir_alloc_nonpg_memory(nvc_stack_size);
			if(vcpu->hv_stack==null)
				goto alloc_failure;
			vcpu->relative_hvm=(noir_vt_hvm_p)hvm->reserved;
		}
	}
	hvm->relative_hvm=(noir_vt_hvm_p)hvm->reserved;
	hvm->relative_hvm->msr_bitmap.virt=noir_alloc_contd_memory(page_size);
	if(hvm->relative_hvm->msr_bitmap.virt)
		hvm->relative_hvm->msr_bitmap.phys=noir_get_physical_address(hvm->relative_hvm->msr_bitmap.virt);
	else
		goto alloc_failure;
	//At this time, we don't need to virtualize I/O instructions. So leave them blank.
	/*hvm->relative_hvm->io_bitmap_a.virt=noir_alloc_contd_memory(page_size);
	hvm->relative_hvm->io_bitmap_b.virt=noir_alloc_contd_memory(page_size);
	if(hvm->relative_hvm->io_bitmap_a.virt && hvm->relative_hvm->io_bitmap_b.virt)
	{
		hvm->relative_hvm->io_bitmap_a.phys=noir_get_physical_address(hvm->relative_hvm->io_bitmap_a.virt);
		hvm->relative_hvm->io_bitmap_b.phys=noir_get_physical_address(hvm->relative_hvm->io_bitmap_b.virt);
	}*/
	hvm->relative_hvm->msr_auto_list.virt=noir_alloc_contd_memory(page_size);
	if(hvm->relative_hvm->msr_auto_list.virt)
		hvm->relative_hvm->msr_auto_list.phys=noir_get_physical_address(hvm->relative_hvm->msr_auto_list.virt);
	else
		goto alloc_failure;
	if(hvm->virtual_cpu==null)goto alloc_failure;
	nv_dprintf("All allocations are done, start subversion!\n");
	nvc_vt_setup_msr_hook(hvm);
	noir_generic_call(nvc_vt_subvert_processor_thunk,hvm->virtual_cpu);
	return noir_success;
alloc_failure:
	nv_dprintf("Allocation failure!\n");
	nvc_vt_cleanup(hvm);
	return noir_insufficient_resources;
}

void static nvc_vt_restore_processor(noir_vt_vcpu_p vcpu)
{
	;
}

void static nvc_vt_restore_processor_thunk(void* context,u32 processor_id)
{
	noir_vt_vcpu_p vcpu=(noir_vt_vcpu_p)context;
	nvc_vt_restore_processor(&vcpu[processor_id]);
}

void nvc_vt_restore_system(noir_hypervisor_p hvm)
{
	if(hvm->virtual_cpu)
	{
		noir_generic_call(nvc_vt_restore_processor_thunk,hvm->virtual_cpu);
		nvc_vt_cleanup(hvm);
	}
}