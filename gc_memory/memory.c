/*
 * Mupen64 - memory.c
 * Copyright (C) 2002 Hacktarux
 *
 * Mupen64 homepage: http://mupen64.emulation64.com
 * email address: hacktarux@yahoo.fr
 * 
 * If you want to contribute to the project please contact
 * me first (maybe someone is already making what you are
 * planning to do).
 *
 *
 * This program is free software; you can redistribute it and/
 * or modify it under the terms of the GNU General Public Li-
 * cence as published by the Free Software Foundation; either
 * version 2 of the Licence, or any later version.
 *
 * This program is distributed in the hope that it will be use-
 * ful, but WITHOUT ANY WARRANTY; without even the implied war-
 * ranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public Licence for more details.
 *
 * You should have received a copy of the GNU General Public
 * Licence along with this program; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139,
 * USA.
 *
**/
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

#include "../config.h"

#include <stdio.h>
#include <stdlib.h>
#ifndef __WIN32__
#include "../main/winlnxdefs.h"
#else
#include <windows.h>
#endif

#include "memory.h"
#include "dma.h"
#include "../main/ROM-Cache.h"
#include "TLB-Cache.h"
#include "Saves.h"
#include "../r4300/r4300.h"
#include "../r4300/macros.h"
#include "../r4300/interupt.h"
#include "../r4300/Invalid_Code.h"
#include "pif.h"
#include "flashram.h"
#include "../main/plugin.h"
#include "../main/guifuncs.h"

#include "../gui/DEBUG.h"
#include <assert.h>

/* definitions of the rcp's structures and memory area */
RDRAM_register rdram_register;
mips_register MI_register;
PI_register pi_register;
SP_register sp_register;
RSP_register rsp_register;
SI_register si_register;
VI_register vi_register;
RI_register ri_register;
AI_register ai_register;
DPC_register dpc_register;
DPS_register dps_register;

#ifdef USE_EXPANSION
	#define MEM_SIZE 0x800000
	#define MEM_TBL_SIZE 0x80
	#define MEMMASK 0x7FFFFF
#else
	#define MEM_SIZE 0x400000
	#define MEM_TBL_SIZE 0x40
	#define MEMMASK 0x3FFFFF
#endif

unsigned long rdram[MEM_SIZE/4]  __attribute__((aligned(32)));
unsigned char *const rdramb = (unsigned char *)(rdram);
unsigned long SP_DMEM[0x1000/4*2] __attribute__((aligned(32)));
unsigned long *const SP_IMEM = SP_DMEM+0x1000/4;
unsigned char *const SP_DMEMb = (unsigned char *)(SP_DMEM);
unsigned char *const SP_IMEMb = (unsigned char*)(SP_DMEM+0x1000/4);
unsigned long PIF_RAM[0x40/4] __attribute__((aligned(32)));
unsigned char *const PIF_RAMb = (unsigned char *)(PIF_RAM);

// values that are being written are stored in these variables
unsigned long word;
unsigned char byte;
unsigned short hword;
unsigned long long int dword;

// address : address of the read/write operation being done
unsigned long address = 0;
// *address_low = the lower 16 bit of the address :
#ifdef _BIG_ENDIAN
static unsigned short *const address_low = (unsigned short *)(&address)+1; 
#else
static unsigned short *const address_low = (unsigned short *)(&address);
#endif

// trash : when we write to unmaped memory it is written here
static unsigned long trash;

// hash tables of memory functions
void (**rwmem[0x10000])();

void (*rw_nothing[8])() =
	{ read_nothing,  read_nothingb,  read_nothingh,  read_nothingd,
	  write_nothing, write_nothingb, write_nothingh, write_nothingd };

void (*rw_nomem[8])() =
	{ read_nomem,  read_nomemb,  read_nomemh,  read_nomemd,
	  write_nomem, write_nomemb, write_nomemh, write_nomemd };

void (*rw_rdram[8])() =
	{ read_rdram,  read_rdramb,  read_rdramh,  read_rdramd,
	  write_rdram, write_rdramb, write_rdramh, write_rdramd };

void (*rw_rdramFB[8])() =
	{ read_rdramFB,  read_rdramFBb,  read_rdramFBh,  read_rdramFBd,
	  write_rdramFB, write_rdramFBb, write_rdramFBh, write_rdramFBd };

void (*rw_rdramreg[8])() =
	{ read_rdramreg,  read_rdramregb,  read_rdramregh,  read_rdramregd,
	  write_rdramreg, write_rdramregb, write_rdramregh, write_rdramregd };

void (*rw_rsp_mem[8])() =
	{ read_rsp_mem,  read_rsp_memb,  read_rsp_memh,  read_rsp_memd,
	  write_rsp_mem, write_rsp_memb, write_rsp_memh, write_rsp_memd };

void (*rw_rsp_reg[8])() =
	{ read_rsp_reg,  read_rsp_regb,  read_rsp_regh,  read_rsp_regd,
	  write_rsp_reg, write_rsp_regb, write_rsp_regh, write_rsp_regd };

void (*rw_rsp[8])() =
	{ read_rsp,  read_rspb,  read_rsph,  read_rspd,
	  write_rsp, write_rspb, write_rsph, write_rspd };

void (*rw_dp[8])() =
	{ read_dp,  read_dpb,  read_dph,  read_dpd,
	  write_dp, write_dpb, write_dph, write_dpd };

void (*rw_dps[8])() =
	{ read_dps,  read_dpsb,  read_dpsh,  read_dpsd,
	  write_dps, write_dpsb, write_dpsh, write_dpsd };

void (*rw_mi[8])() =
	{ read_mi,  read_mib,  read_mih,  read_mid,
	  write_mi, write_mib, write_mih, write_mid };

void (*rw_vi[8])() =
	{ read_vi,  read_vib,  read_vih,  read_vid,
	  write_vi, write_vib, write_vih, write_vid };

void (*rw_ai[8])() =
	{ read_ai,  read_aib,  read_aih,  read_aid,
	  write_ai, write_aib, write_aih, write_aid };

void (*rw_pi[8])() =
	{ read_pi,  read_pib,  read_pih,  read_pid,
	  write_pi, write_pib, write_pih, write_pid };

void (*rw_ri[8])() =
	{ read_ri,  read_rib,  read_rih,  read_rid,
	  write_ri, write_rib, write_rih, write_rid };

void (*rw_si[8])() =
	{ read_si,  read_sib,  read_sih,  read_sid,
	  write_si, write_sib, write_sih, write_sid };

void (*rw_flashram0[8])() =
	{ read_flashram_status,  read_flashram_statusb,
	  read_flashram_statush, read_flashram_statusd,
	  write_flashram_dummy,  write_flashram_dummyb,
	  write_flashram_dummyh, write_flashram_dummyd };

void (*rw_flashram1[8])() =
	{ read_nothing,            read_nothingb,
	  read_nothingh,           read_nothingd,
	  write_flashram_command,  write_flashram_commandb,
	  write_flashram_commandh, write_flashram_commandd };

void (*rw_rom0[8])() =
	{ read_rom,      read_romb,      read_romh,      read_romd,
	  write_nothing, write_nothingb, write_nothingh, write_nothingd };

void (*rw_rom1[8])() =
	{ read_rom,      read_romb,      read_romh,      read_romd,
	  write_rom,     write_nothingb, write_nothingh, write_nothingd };

void (*rw_pif[8])() =
	{ read_pif,  read_pifb,  read_pifh,  read_pifd,
	  write_pif, write_pifb, write_pifh, write_pifd };


// memory sections
static unsigned long *readrdramreg[0x28];
static unsigned long *readrspreg[0x30];
static unsigned long *readrsp[0x10];
static unsigned long *readmi[0x20];
static unsigned long *readvi[0x40];
static unsigned long *readai[0x20];
static unsigned long *readpi[0x34];
static unsigned long *readri[0x30];
static unsigned long *readsi[0x20];
static unsigned long *readdp[0x30];
static unsigned long *readdps[0x20];

// the frameBufferInfos
static FrameBufferInfo frameBufferInfos[6];
static char framebufferRead[0x800];
static int firstFrameBufferSetting;

int init_memory()
{
	int i;
      
	//init hash tables
	for (i=0; i<(0x10000); i++) rwmem[i] = rw_nomem;
   
	//init RDRAM
	for (i=0; i<(MEM_SIZE/4); i++) rdram[i]=0;

	for (i=0; i<MEM_TBL_SIZE; i++)
	{
		rwmem[(0x8000+i)] = rw_rdram;
		rwmem[(0xa000+i)] = rw_rdram;
	}
	for (i=MEM_TBL_SIZE; i<0x3F0; i++)
    {
		rwmem[0x8000+i] = rw_nothing;
		rwmem[0xa000+i] = rw_nothing;
	}
   
	//init RDRAM registers
	rwmem[0x83f0] = rw_rdramreg;
	rwmem[0xa3f0] = rw_rdramreg;
	memset(&rdram_register, 0, sizeof(RDRAM_register));
	readrdramreg[0x0] = &rdram_register.rdram_config;
	readrdramreg[0x4] = &rdram_register.rdram_device_id;
	readrdramreg[0x8] = &rdram_register.rdram_delay;
	readrdramreg[0xc] = &rdram_register.rdram_mode;
	readrdramreg[0x10] = &rdram_register.rdram_ref_interval;
	readrdramreg[0x14] = &rdram_register.rdram_ref_row;
	readrdramreg[0x18] = &rdram_register.rdram_ras_interval;
	readrdramreg[0x1c] = &rdram_register.rdram_min_interval;
	readrdramreg[0x20] = &rdram_register.rdram_addr_select;
	readrdramreg[0x24] = &rdram_register.rdram_device_manuf;
   
	for (i=1; i<0x10; i++)
	{
		rwmem[0x83f0+i] = rw_nothing;
		rwmem[0xa3f0+i] = rw_nothing;
	}
   
	//init RSP memory
	rwmem[0x8400] = rw_rsp_mem;
	rwmem[0xa400] = rw_rsp_mem;
	for (i=0; i<(0x1000/4); i++) SP_DMEM[i]=0;
	for (i=0; i<(0x1000/4); i++) SP_IMEM[i]=0;
   
	for (i=1; i<0x4; i++)
	{
		rwmem[0x8400+i] = rw_nothing;
		rwmem[0xa400+i] = rw_nothing;
	}
   
	//init RSP registers
	rwmem[0x8404] = rw_rsp_reg;
	rwmem[0xa404] = rw_rsp_reg;
	memset(&sp_register, 0, sizeof(SP_register));
	sp_register.sp_status_reg=1;
	sp_register.halt=1;
	readrspreg[0x0] = &sp_register.sp_mem_addr_reg;
	readrspreg[0x4] = &sp_register.sp_dram_addr_reg;
	readrspreg[0x8] = &sp_register.sp_rd_len_reg;
	readrspreg[0xc] = &sp_register.sp_wr_len_reg;
	readrspreg[0x10] = &sp_register.sp_status_reg;
	readrspreg[0x14] = &sp_register.sp_dma_full_reg;
	readrspreg[0x18] = &sp_register.sp_dma_busy_reg;
	readrspreg[0x1c] = &sp_register.sp_semaphore_reg;

	for (i=0x20; i<0x30; i++) readrspreg[i] = &trash;
	for (i=5; i<8; i++)
	{
		rwmem[0x8400+i] = rw_nothing;
		rwmem[0xa400+i] = rw_nothing;
	}
   
	rwmem[0x8408] = rw_rsp;
	rwmem[0xa408] = rw_rsp;
	memset(&rsp_register, 0, sizeof(RSP_register));
	readrsp[0x0] = &rsp_register.rsp_pc;
	readrsp[0x4] = &rsp_register.rsp_ibist;

	for (i=0x8; i<0x10; i++) readrsp[i] = &trash;
	for (i=9; i<0x10; i++)
	{
		rwmem[0x8400+i] = rw_nothing;
		rwmem[0xa400+i] = rw_nothing;
	}
   
	//init rdp command registers
	rwmem[0x8410] = rw_dp;
	rwmem[0xa410] = rw_dp;
	memset(&dpc_register, 0, sizeof(DPC_register));
	readdp[0x0] = &dpc_register.dpc_start;
	readdp[0x4] = &dpc_register.dpc_end;
	readdp[0x8] = &dpc_register.dpc_current;
	readdp[0xc] = &dpc_register.dpc_status;
	readdp[0x10] = &dpc_register.dpc_clock;
	readdp[0x14] = &dpc_register.dpc_bufbusy;
	readdp[0x18] = &dpc_register.dpc_pipebusy;
	readdp[0x1c] = &dpc_register.dpc_tmem;

	for (i=0x20; i<0x30; i++) readdp[i] = &trash;
	for (i=1; i<0x10; i++)
	{
		rwmem[0x8410+i] = rw_nothing;
		rwmem[0xa410+i] = rw_nothing;
	}
   
	//init rsp span registers
	rwmem[0x8420] = rw_dps;
	rwmem[0xa420] = rw_dps;
	memset(&dps_register, 0, sizeof(DPS_register));
	readdps[0x0] = &dps_register.dps_tbist;
	readdps[0x4] = &dps_register.dps_test_mode;
	readdps[0x8] = &dps_register.dps_buftest_addr;
	readdps[0xc] = &dps_register.dps_buftest_data;
   
	for (i=0x10; i<0x20; i++) readdps[i] = &trash;
	for (i=1; i<0x10; i++)
	{
		rwmem[0x8420+i] = rw_nothing;
		rwmem[0xa420+i] = rw_nothing;
	}
   
	//init mips registers
	rwmem[0xa830] = rw_mi;
	rwmem[0xa430] = rw_mi;
	memset(&MI_register, 0, sizeof(mips_register));
	MI_register.mi_version_reg = 0x02020102;
	readmi[0x0] = &MI_register.mi_init_mode_reg;
	readmi[0x4] = &MI_register.mi_version_reg;
	readmi[0x8] = &MI_register.mi_intr_reg;
	readmi[0xc] = &MI_register.mi_intr_mask_reg;

	for (i=0x10; i<0x20; i++) readmi[i] = &trash;
	for (i=1; i<0x10; i++)
	{
		rwmem[0x8430+i] = rw_nothing;
		rwmem[0xa430+i] = rw_nothing;
	}
   
	//init VI registers
	rwmem[0x8440] = rw_vi;
	rwmem[0xa440] = rw_vi;
	memset(&vi_register, 0, sizeof(VI_register));
	readvi[0x0] = &vi_register.vi_status;
	readvi[0x4] = &vi_register.vi_origin;
	readvi[0x8] = &vi_register.vi_width;
	readvi[0xc] = &vi_register.vi_v_intr;
	readvi[0x10] = &vi_register.vi_current;
	readvi[0x14] = &vi_register.vi_burst;
	readvi[0x18] = &vi_register.vi_v_sync;
	readvi[0x1c] = &vi_register.vi_h_sync;
	readvi[0x20] = &vi_register.vi_leap;
	readvi[0x24] = &vi_register.vi_h_start;
	readvi[0x28] = &vi_register.vi_v_start;
	readvi[0x2c] = &vi_register.vi_v_burst;
	readvi[0x30] = &vi_register.vi_x_scale;
	readvi[0x34] = &vi_register.vi_y_scale;

	for (i=0x38; i<0x40; i++) readvi[i] = &trash;
	for (i=1; i<0x10; i++)
	{
		rwmem[0x8440+i] = rw_nothing;
		rwmem[0xa440+i] = rw_nothing;
	}
   
	//init AI registers
	rwmem[0x8450] = rw_ai;
	rwmem[0xa450] = rw_ai;
	memset(&ai_register, 0, sizeof(AI_register));
	readai[0x0] = &ai_register.ai_dram_addr;
	readai[0x4] = &ai_register.ai_len;
	readai[0x8] = &ai_register.ai_control;
	readai[0xc] = &ai_register.ai_status;
	readai[0x10] = &ai_register.ai_dacrate;
	readai[0x14] = &ai_register.ai_bitrate;

	for (i=0x18; i<0x20; i++) readai[i] = &trash;
	for (i=1; i<0x10; i++)
	{
		rwmem[0x8450+i] = rw_nothing;
		rwmem[0xa450+i] = rw_nothing;
	}
   
	//init PI registers
	rwmem[0x8460] = rw_pi;
	rwmem[0xa460] = rw_pi;
	memset(&pi_register, 0, sizeof(PI_register));
	readpi[0x0] = &pi_register.pi_dram_addr_reg;
	readpi[0x4] = &pi_register.pi_cart_addr_reg;
	readpi[0x8] = &pi_register.pi_rd_len_reg;
	readpi[0xc] = &pi_register.pi_wr_len_reg;
	readpi[0x10] = &pi_register.read_pi_status_reg;
	readpi[0x14] = &pi_register.pi_bsd_dom1_lat_reg;
	readpi[0x18] = &pi_register.pi_bsd_dom1_pwd_reg;
	readpi[0x1c] = &pi_register.pi_bsd_dom1_pgs_reg;
	readpi[0x20] = &pi_register.pi_bsd_dom1_rls_reg;
	readpi[0x24] = &pi_register.pi_bsd_dom2_lat_reg;
	readpi[0x28] = &pi_register.pi_bsd_dom2_pwd_reg;
	readpi[0x2c] = &pi_register.pi_bsd_dom2_pgs_reg;
	readpi[0x30] = &pi_register.pi_bsd_dom2_rls_reg;

	for (i=1; i<0x10; i++)
	{
		rwmem[0x8460+i] = rw_nothing;
		rwmem[0xa460+i] = rw_nothing;
	}
   
	//init RI registers
	rwmem[0x8470] = rw_ri;
	rwmem[0xa470] = rw_ri;
	memset(&ri_register, 0, sizeof(RI_register));
	readri[0x0] = &ri_register.ri_mode;
	readri[0x4] = &ri_register.ri_config;
	readri[0x8] = &ri_register.ri_current_load;
	readri[0xc] = &ri_register.ri_select;
	readri[0x10] = &ri_register.ri_refresh;
	readri[0x14] = &ri_register.ri_latency;
	readri[0x18] = &ri_register.ri_error;
	readri[0x1c] = &ri_register.ri_werror;
   
	for (i=0x20; i<0x30; i++) readri[i] = &trash;
	for (i=1; i<0x10; i++)
	{
		rwmem[0x8470+i] = rw_nothing;
		rwmem[0xa470+i] = rw_nothing;
	}
   
	//init SI registers
	rwmem[0x8480] = rw_si;
	rwmem[0xa480] = rw_si;
	memset(&si_register, 0, sizeof(SI_register));
	readsi[0x00] = &si_register.si_dram_addr;
	readsi[0x04] = &si_register.si_pif_addr_rd64b;
	readsi[0x08] = &trash;
	readsi[0x0C] = &trash;
	readsi[0x10] = &si_register.si_pif_addr_wr64b;
	readsi[0x14] = &trash;
	readsi[0x18] = &si_register.si_status;

	for (i=0x1c; i<0x20; i++) readsi[i] = &trash;
	for (i=0x481; i<0x800; i++)
	{
		rwmem[0x8000+i] = rw_nothing;
		rwmem[0xa000+i] = rw_nothing;
	}
   
	//init flashram / sram
	rwmem[0x8800] = rw_flashram0;
	rwmem[0xa800] = rw_flashram0;
	rwmem[0x8801] = rw_flashram1;
	rwmem[0xa801] = rw_flashram1;

	for (i=0x802; i<0x1000; i++)
	{
		rwmem[0x8000+i] = rw_nothing;
		rwmem[0xa000+i] = rw_nothing;
	}
   
	//init rom area
	for (i=0; i<(rom_length >> 16); i++) 
	{
		rwmem[0x9000+i] = rw_rom0;
		rwmem[0xb000+i] = rw_rom1;
	}
	for (i=(rom_length >> 16); i<0xfc0; i++) 
	{
		rwmem[0x9000+i] = rw_nothing;
		rwmem[0xb000+i] = rw_nothing;
	}

	//init PIF_RAM
	rwmem[0x9fc0] = rw_pif;
	rwmem[0xbfc0] = rw_pif;
	for (i=0; i<(0x40/4); i++) PIF_RAM[i]=0;

	for (i=0xfc1; i<0x1000; i++) 
	{
		rwmem[0x9000+i] = rw_nothing;
		rwmem[0xb000+i] = rw_nothing;
	}

	flashRAMInfo.use_flashram = 0;
	init_flashram();

	frameBufferInfos[0].addr = 0;
	firstFrameBufferSetting = 1;

	//printf("memory initialized\n");
	return 0;
}

void free_memory()
{
#ifdef USE_TLB_CACHE
	TLBCache_deinit();
#endif
}

static void update_MI_init_mode_reg()
{
	MI_register.mi_init_mode_reg &= ~0x7F; // init_length
	MI_register.mi_init_mode_reg |= MI_register.w_mi_init_mode_reg & 0x7F;
	if (MI_register.w_mi_init_mode_reg & 0x80) // clear init_mode
		MI_register.mi_init_mode_reg &= ~0x80;
	if (MI_register.w_mi_init_mode_reg & 0x100) // set init_mode
		MI_register.mi_init_mode_reg |= 0x80;
	if (MI_register.w_mi_init_mode_reg & 0x200) // clear ebus_test_mode
		MI_register.mi_init_mode_reg &= ~0x100;
	if (MI_register.w_mi_init_mode_reg & 0x400) // set ebus_test_mode
		MI_register.mi_init_mode_reg |= 0x100;
	if (MI_register.w_mi_init_mode_reg & 0x800) // clear DP interupt
	{
		MI_register.mi_intr_reg &= 0xFFFFFFDF;
		check_interupt();
	}
	if (MI_register.w_mi_init_mode_reg & 0x1000) // clear RDRAM_reg_mode
		MI_register.mi_init_mode_reg &= ~0x200;
	if (MI_register.w_mi_init_mode_reg & 0x2000) // set RDRAM_reg_mode
		MI_register.mi_init_mode_reg |= 0x200;
}

static void update_MI_intr_mask_reg()
{
	if (MI_register.w_mi_intr_mask_reg & 0x1)   MI_register.mi_intr_mask_reg &= ~0x1; // clear SP mask
	if (MI_register.w_mi_intr_mask_reg & 0x2)   MI_register.mi_intr_mask_reg |= 0x1; // set SP mask
	if (MI_register.w_mi_intr_mask_reg & 0x4)   MI_register.mi_intr_mask_reg &= ~0x2; // clear SI mask
	if (MI_register.w_mi_intr_mask_reg & 0x8)   MI_register.mi_intr_mask_reg |= 0x2; // set SI mask
	if (MI_register.w_mi_intr_mask_reg & 0x10)  MI_register.mi_intr_mask_reg &= ~0x4; // clear AI mask
	if (MI_register.w_mi_intr_mask_reg & 0x20)  MI_register.mi_intr_mask_reg |= 0x4; // set AI mask
	if (MI_register.w_mi_intr_mask_reg & 0x40)  MI_register.mi_intr_mask_reg &= ~0x8; // clear VI mask
	if (MI_register.w_mi_intr_mask_reg & 0x80)  MI_register.mi_intr_mask_reg |= 0x8; // set VI mask
	if (MI_register.w_mi_intr_mask_reg & 0x100) MI_register.mi_intr_mask_reg &= ~0x10; // clear PI mask
	if (MI_register.w_mi_intr_mask_reg & 0x200) MI_register.mi_intr_mask_reg |= 0x10; // set PI mask
	if (MI_register.w_mi_intr_mask_reg & 0x400) MI_register.mi_intr_mask_reg &= ~0x20; // clear DP mask
	if (MI_register.w_mi_intr_mask_reg & 0x800) MI_register.mi_intr_mask_reg |= 0x20; // set DP mask
}

static void do_SP_task()
{
	int save_pc = rsp_register.rsp_pc & ~0xFFF;
    if (SP_DMEM[0xFC0/4] == 1)
    {
		if (dpc_register.dpc_status & 0x2) // DP frozen (DK64, BC)
        {
            // don't do the task now
            // the task will be done when DP is unfreezed (see update_DPC)
            return;
        }
		// unprotecting old frame buffers
		if(fBGetFrameBufferInfo && fBRead && fBWrite && 
		frameBufferInfos[0].addr)
		{
			int i;
			for(i=0; i<6; i++)
			{
				if(frameBufferInfos[i].addr)
				{
					int j;
					int start = frameBufferInfos[i].addr & MEMMASK;
					int end = start + frameBufferInfos[i].width*
					frameBufferInfos[i].height*
					frameBufferInfos[i].size - 1;
					start = start >> 16;
					end = end >> 16;
					for(j=start; j<=end; j++)
					{
						rwmem[0x8000+j] = rw_rdram;
						rwmem[0xa000+j] = rw_rdram;
					}
				}
			}
		}

		//processDList();
		rsp_register.rsp_pc &= 0xFFF;
		start_section(GFX_SECTION);
		doRspCycles(100);
		end_section(GFX_SECTION);
		rsp_register.rsp_pc |= save_pc;
		new_frame();

		// protecting new frame buffers
		if(fBGetFrameBufferInfo && fBRead && fBWrite) fBGetFrameBufferInfo(frameBufferInfos);
		if(fBGetFrameBufferInfo && fBRead && fBWrite &&
		frameBufferInfos[0].addr)
		{
			int i;
			for(i=0; i<6; i++)
			{
				if(frameBufferInfos[i].addr)
				{
					int j;
					int start = frameBufferInfos[i].addr & MEMMASK;
					int end = start + frameBufferInfos[i].width*
					frameBufferInfos[i].height*
					frameBufferInfos[i].size - 1;
					int start1 = start;
					int end1 = end;
					start >>= 16;
					end >>= 16;
					for(j=start; j<=end; j++)
					{
						rwmem[0x8000+j] = rw_rdramFB;
						rwmem[0xa000+j] = rw_rdramFB;
					}
					start <<= 4;
					end <<= 4;
					for(j=start; j<=end; j++)
					{
						if(j>=start1 && j<=end1) framebufferRead[j]=1;
						else framebufferRead[j] = 0;
					}
					if(firstFrameBufferSetting)
					{
						firstFrameBufferSetting = 0;
						// TODO: fast_memory = 0?
						for(j=0; j<0x100000; j++)
							invalid_code_set(j, 1);
					}
				}
			}
		}
		return;
	}

	if (SP_DMEM[0xFC0/4] == 2)
    {
        //audio.processAList();
        rsp_register.rsp_pc &= 0xFFF;
        start_section(AUDIO_SECTION);
        doRspCycles(100);
        end_section(AUDIO_SECTION);
        rsp_register.rsp_pc |= save_pc;       
    }
    else
    {
        rsp_register.rsp_pc &= 0xFFF;
        doRspCycles(100);
        rsp_register.rsp_pc |= save_pc;
    }
}

void update_SP()
{
	if (sp_register.w_sp_status_reg & 0x1) // clear halt
		sp_register.sp_status_reg &= ~0x1;
	if (sp_register.w_sp_status_reg & 0x2) // set halt
		sp_register.sp_status_reg |= 0x1;
	if (sp_register.w_sp_status_reg & 0x4) // clear broke
		sp_register.sp_status_reg &= ~0x2;
	if (sp_register.w_sp_status_reg & 0x8) // clear SP interupt
	{
		MI_register.mi_intr_reg &= 0xFFFFFFFE;
		check_interupt();
	}
	if (sp_register.w_sp_status_reg & 0x10) // set SP interupt
	{
		MI_register.mi_intr_reg |= 1;
		check_interupt();
	}
	if (sp_register.w_sp_status_reg & 0x20) // clear single step
		sp_register.sp_status_reg &= ~0x20;
	if (sp_register.w_sp_status_reg & 0x40) // set single step
		sp_register.sp_status_reg |= 0x20;
	if (sp_register.w_sp_status_reg & 0x80) // clear interrupt on break
		sp_register.sp_status_reg &= ~0x40;
	if (sp_register.w_sp_status_reg & 0x100) // set interrupt on break
		sp_register.sp_status_reg |= 0x40;
	if (sp_register.w_sp_status_reg & 0x200) // clear signal 0
		sp_register.sp_status_reg &= ~0x80;
	if (sp_register.w_sp_status_reg & 0x400) // set signal 0
		sp_register.sp_status_reg |= 0x80;
	if (sp_register.w_sp_status_reg & 0x800) // clear signal 1
		sp_register.sp_status_reg &= ~0x100;
	if (sp_register.w_sp_status_reg & 0x1000) // set signal 1
		sp_register.sp_status_reg |= 0x100;
	if (sp_register.w_sp_status_reg & 0x2000) // clear signal 2
		sp_register.sp_status_reg &= ~0x200;
	if (sp_register.w_sp_status_reg & 0x4000) // set signal 2
		sp_register.sp_status_reg |= 0x200;
	if (sp_register.w_sp_status_reg & 0x8000) // clear signal 3
		sp_register.sp_status_reg &= ~0x400;
	if (sp_register.w_sp_status_reg & 0x10000) // set signal 3
		sp_register.sp_status_reg |= 0x400;
	if (sp_register.w_sp_status_reg & 0x20000) // clear signal 4
		sp_register.sp_status_reg &= ~0x800;
	if (sp_register.w_sp_status_reg & 0x40000) // set signal 4
		sp_register.sp_status_reg |= 0x800;
	if (sp_register.w_sp_status_reg & 0x80000) // clear signal 5
		sp_register.sp_status_reg &= ~0x1000;
	if (sp_register.w_sp_status_reg & 0x100000) // set signal 5
		sp_register.sp_status_reg |= 0x1000;
	if (sp_register.w_sp_status_reg & 0x200000) // clear signal 6
		sp_register.sp_status_reg &= ~0x2000;
	if (sp_register.w_sp_status_reg & 0x400000) // set signal 6
		sp_register.sp_status_reg |= 0x2000;
	if (sp_register.w_sp_status_reg & 0x800000) // clear signal 7
		sp_register.sp_status_reg &= ~0x4000;
	if (sp_register.w_sp_status_reg & 0x1000000) // set signal 7
		sp_register.sp_status_reg |= 0x4000;

	if (!(sp_register.w_sp_status_reg & 0x1) && 
		!(sp_register.w_sp_status_reg & 0x4)) return;
	if (!(sp_register.sp_status_reg & 0x3)) // !halt && !broke
		do_SP_task();
}

void update_DPC(void)
{
	if (dpc_register.w_dpc_status & 0x1) // clear xbus_dmem_dma
		dpc_register.dpc_status &= ~0x1;
	if (dpc_register.w_dpc_status & 0x2) // set xbus_dmem_dma
		dpc_register.dpc_status |= 0x1;
	if (dpc_register.w_dpc_status & 0x4) // clear freeze
	{
		dpc_register.dpc_status &= ~0x2;
		// see do_SP_task for more info
		if (!(sp_register.sp_status_reg & 0x3)) // !halt && !broke
			do_SP_task();
	}
	if (dpc_register.w_dpc_status & 0x8) // set freeze
		dpc_register.dpc_status |= 0x2;
	if (dpc_register.w_dpc_status & 0x10) // clear flush
		dpc_register.dpc_status &= ~0x4;
	if (dpc_register.w_dpc_status & 0x20) // set flush
		dpc_register.dpc_status |= 0x4;
}

void read_nothing()
{
   word = (*address_low << 16) | *address_low;
}

void read_nothingb()
{
   byte = *address_low;
}

void read_nothingh()
{
   hword = *address_low;
}

void read_nothingd()
{
   dword = (*address_low << 16) | *address_low;
}

void write_nothing()
{
}

void write_nothingb()
{
}

void write_nothingh()
{
}

void write_nothingd()
{
}

void read_nomem()
{
   address = virtual_to_physical_address(address,0);
   if (address == 0x00000000) return;
   read_word_in_memory();
}

void read_nomemb()
{
   address = virtual_to_physical_address(address,0);
   if (address == 0x00000000) return;
   read_byte_in_memory();
}

void read_nomemh()
{
   address = virtual_to_physical_address(address,0);
   if (address == 0x00000000) return;
   read_hword_in_memory();
}

void read_nomemd()
{
   address = virtual_to_physical_address(address,0);
   if (address == 0x00000000) return;
   read_dword_in_memory();
}

void write_nomem()
{
   address = virtual_to_physical_address(address,1);
   if (address == 0x00000000) return;
   write_word_in_memory();
}

void write_nomemb()
{
   address = virtual_to_physical_address(address,1);
   if (address == 0x00000000) return;
   write_byte_in_memory();
}

void write_nomemh()
{
   address = virtual_to_physical_address(address,1);
   if (address == 0x00000000) return;
   write_hword_in_memory();
}

void write_nomemd()
{
   address = virtual_to_physical_address(address,1);
   if (address == 0x00000000) return;
   write_dword_in_memory();
}

void read_rdram()
{
   word = *((unsigned long *)(rdramb + (address & MEMMASK)));
}

void read_rdramb()
{
   byte = *(rdramb + ((address & MEMMASK)^S8));
}

void read_rdramh()
{
   hword = *((unsigned short *)(rdramb + ((address & MEMMASK)^S16)));
}

void read_rdramd()
{
   dword = ((unsigned long long int)(*(unsigned long *)(rdramb + (address & MEMMASK))) << 32) |
     ((*(unsigned long *)(rdramb + (address & MEMMASK) + 4)));
}

void read_rdramFB()
{
	int i;
	for(i=0; i<6; i++)
	{
		if(frameBufferInfos[i].addr)
		{
			int start = frameBufferInfos[i].addr & MEMMASK;
			int end = start + frameBufferInfos[i].width*
			frameBufferInfos[i].height*
			frameBufferInfos[i].size - 1;
			if((address & MEMMASK) >= start && (address & MEMMASK) <= end && framebufferRead[(address & MEMMASK)>>12])
			{
				fBRead(address);
				framebufferRead[(address & MEMMASK)>>12] = 0;
			}
		}
	}
	read_rdram();
}

void read_rdramFBb()
{
	int i;
	for(i=0; i<6; i++)
	{
		if(frameBufferInfos[i].addr)
		{
			int start = frameBufferInfos[i].addr & MEMMASK;
			int end = start + frameBufferInfos[i].width*
			frameBufferInfos[i].height*
			frameBufferInfos[i].size - 1;
			if((address & MEMMASK) >= start && (address & MEMMASK) <= end && framebufferRead[(address & MEMMASK)>>12])
			{
				fBRead(address);
				framebufferRead[(address & MEMMASK)>>12] = 0;
			}
		}
	}
	read_rdramb();
}

void read_rdramFBh()
{
	int i;
	for(i=0; i<6; i++)
	{
		if(frameBufferInfos[i].addr)
		{
			int start = frameBufferInfos[i].addr & MEMMASK;
			int end = start + frameBufferInfos[i].width*
			frameBufferInfos[i].height*
			frameBufferInfos[i].size - 1;
			if((address & MEMMASK) >= start && (address & MEMMASK) <= end && framebufferRead[(address & MEMMASK)>>12])
			{
				fBRead(address);
				framebufferRead[(address & MEMMASK)>>12] = 0;
			}
		}
	}
	read_rdramh();
}

void read_rdramFBd()
{
	int i;
	for(i=0; i<6; i++)
	{
		if(frameBufferInfos[i].addr)
		{
			int start = frameBufferInfos[i].addr & MEMMASK;
			int end = start + frameBufferInfos[i].width*
			frameBufferInfos[i].height*
			frameBufferInfos[i].size - 1;
			if((address & MEMMASK) >= start && (address & MEMMASK) <= end && framebufferRead[(address & MEMMASK)>>12])
			{
				fBRead(address);
				framebufferRead[(address & MEMMASK)>>12] = 0;
			}
		}
	}
	read_rdramd();
}

void write_rdram()
{
   *((unsigned long *)(rdramb + (address & MEMMASK))) = word;
}

void write_rdramb()
{
   *((rdramb + ((address & MEMMASK)^S8))) = byte;
}

void write_rdramh()
{
   *(unsigned short *)((rdramb + ((address & MEMMASK)^S16))) = hword;
}

void write_rdramd()
{
   *((unsigned long *)(rdramb + (address & MEMMASK))) = dword >> 32;
   *((unsigned long *)(rdramb + (address & MEMMASK) + 4 )) = dword & 0xFFFFFFFF;
}

void write_rdramFB()
{
	int i;
	for(i=0; i<6; i++)
	{
		if(frameBufferInfos[i].addr)
		{
			int start = frameBufferInfos[i].addr & MEMMASK;
			int end = start + frameBufferInfos[i].width*
			frameBufferInfos[i].height*
			frameBufferInfos[i].size - 1;
			if((address & MEMMASK) >= start && (address & MEMMASK) <= end)
				fBWrite(address, 4);
		}
	}
	write_rdram();
}

void write_rdramFBb()
{
	int i;
	for(i=0; i<6; i++)
	{
		if(frameBufferInfos[i].addr)
		{
			int start = frameBufferInfos[i].addr & MEMMASK;
			int end = start + frameBufferInfos[i].width*
			frameBufferInfos[i].height*
			frameBufferInfos[i].size - 1;
			if((address & MEMMASK) >= start && (address & MEMMASK) <= end)
				fBWrite(address^S8, 1);
		}
	}
	write_rdramb();
}

void write_rdramFBh()
{
	int i;
	for(i=0; i<6; i++)
	{
		if(frameBufferInfos[i].addr)
		{
			int start = frameBufferInfos[i].addr & MEMMASK;
			int end = start + frameBufferInfos[i].width*
			frameBufferInfos[i].height*
			frameBufferInfos[i].size - 1;
			if((address & MEMMASK) >= start && (address & MEMMASK) <= end)
				fBWrite(address^S16, 2);
		}
	}
	write_rdramh();
}

void write_rdramFBd()
{
	int i;
	for(i=0; i<6; i++)
	{
		if(frameBufferInfos[i].addr)
		{
			int start = frameBufferInfos[i].addr & MEMMASK;
			int end = start + frameBufferInfos[i].width*
			frameBufferInfos[i].height*
			frameBufferInfos[i].size - 1;
			if((address & MEMMASK) >= start && (address & MEMMASK) <= end)
				fBWrite(address, 8);
		}
	}
	write_rdramd();
}

void read_rdramreg()
{
	if(*address_low >= sizeof(readrdramreg)) {
		word = trash;
	}
	else {
		word = *(readrdramreg[*address_low]);
	}
}

void read_rdramregb()
{
	if((*address_low & 0xfffc) >= sizeof(readrdramreg)) {
		byte = trash & 0xFF;
	}
	else {
		byte = *((unsigned char*)readrdramreg[*address_low & 0xfffc]
			+ ((*address_low&3)^S8) );
	}
}

void read_rdramregh()
{
	if((*address_low & 0xfffc) >= sizeof(readrdramreg)) {
		hword = trash & 0xFFFF;
	}
	else {
		hword = *((unsigned short*)((unsigned char*)readrdramreg[*address_low & 0xfffc]
			+ ((*address_low&3)^S16) ));
	}
}

void read_rdramregd()
{
	if(*address_low > sizeof(readrdramreg) - sizeof(long long int)) {
		dword = trash;
	}
	else {
		dword = ((unsigned long long int)(*readrdramreg[*address_low])<<32) |
			*readrdramreg[*address_low + 4];
	}
}

void write_rdramreg()
{
	if(*address_low >= sizeof(readrdramreg)) {
		trash = word;
	}
	else {
		*readrdramreg[*address_low] = word;
	}
}

void write_rdramregb()
{
	if((*address_low & 0xfffc) >= sizeof(readrdramreg)) {
		trash = byte;
	}
	else {
		*((unsigned char*)readrdramreg[*address_low & 0xfffc]
			+ ((*address_low&3)^S8) ) = byte;
	}
}

void write_rdramregh()
{
	if((*address_low & 0xfffc) >= sizeof(readrdramreg)) {
		trash = hword;
	}
	else {
		*((unsigned short*)((unsigned char*)readrdramreg[*address_low & 0xfffc]
			+ ((*address_low&3)^S16) )) = hword;
	}
}

void write_rdramregd()
{
	if(*address_low > sizeof(readrdramreg) - sizeof(long long int)) {
		trash = dword;
	}
	else {
		*readrdramreg[*address_low] = dword >> 32;
		*readrdramreg[*address_low+4] = dword & 0xFFFFFFFF;
	}
}

void read_rsp_mem()
{
	if (*address_low < 0x1000)
		word = *((unsigned long *)(SP_DMEMb + (*address_low)));
	else if (*address_low < 0x2000)
		word = *((unsigned long *)(SP_IMEMb + (*address_low&0xFFF)));
	else
		read_nomem();
}

void read_rsp_memb()
{
	if (*address_low < 0x1000)
		byte = *(SP_DMEMb + (*address_low^S8));
	else if (*address_low < 0x2000)
		byte = *(SP_IMEMb + ((*address_low&0xFFF)^S8));
	else
		read_nomemb();
}

void read_rsp_memh()
{
	if (*address_low < 0x1000)
		hword = *((unsigned short *)(SP_DMEMb + (*address_low^S16)));
	else if (*address_low < 0x2000)
		hword = *((unsigned short *)(SP_IMEMb + ((*address_low&0xFFF)^S16)));
	else
		read_nomemh();
}

void read_rsp_memd()
{
	if (*address_low < 0x1000)
	{
		dword = ((unsigned long long int)(*(unsigned long *)(SP_DMEMb + (*address_low))) << 32) | ((*(unsigned long *)(SP_DMEMb + (*address_low) + 4)));
	}
	else if (*address_low < 0x2000)
	{
		dword = ((unsigned long long int)(*(unsigned long *)(SP_IMEMb + (*address_low&0xFFF))) << 32) | ((*(unsigned long *)(SP_IMEMb + (*address_low&0xFFF) + 4)));
	}
	else
		read_nomemd();
}

void write_rsp_mem()
{
	if (*address_low < 0x1000)
		*((unsigned long *)(SP_DMEMb + (*address_low))) = word;
	else if (*address_low < 0x2000)
		*((unsigned long *)(SP_IMEMb + (*address_low&0xFFF))) = word;
	else
		write_nomem();
}

void write_rsp_memb()
{
	if (*address_low < 0x1000)
		*(SP_DMEMb + (*address_low^S8)) = byte;
	else if (*address_low < 0x2000)
		*(SP_IMEMb + ((*address_low&0xFFF)^S8)) = byte;
	else
		write_nomemb();
}

void write_rsp_memh()
{
	if (*address_low < 0x1000)
		*((unsigned short *)(SP_DMEMb + (*address_low^S16))) = hword;
	else if (*address_low < 0x2000)
		*((unsigned short *)(SP_IMEMb + ((*address_low&0xFFF)^S16))) = hword;
	else
		write_nomemh();
}

void write_rsp_memd()
{
	if (*address_low < 0x1000)
	{
		*((unsigned long *)(SP_DMEMb + *address_low)) = dword >> 32;
		*((unsigned long *)(SP_DMEMb + *address_low + 4 )) = dword & 0xFFFFFFFF;
	}
	else if (*address_low < 0x2000)
	{
		*((unsigned long *)(SP_IMEMb + (*address_low&0xFFF))) = dword >> 32;
		*((unsigned long *)(SP_IMEMb + (*address_low&0xFFF) + 4 )) = dword & 0xFFFFFFFF;
	}
	else
		read_nomemd();
}

void read_rsp_reg()
{
	if(*address_low == 0x1C)
		sp_register.sp_semaphore_reg = 1;
	word = *(readrspreg[*address_low]);
}

void read_rsp_regb()
{
	switch(*address_low)
	{
		case 0x1c:
		case 0x1d:
		case 0x1e:
		case 0x1f:
			sp_register.sp_semaphore_reg = 1;
		break;
	}
	byte = *((unsigned char*)readrspreg[*address_low & 0xfffc] + ((*address_low&3)^S8) );
}

void read_rsp_regh()
{
	switch(*address_low)
	{
		case 0x1c:
		case 0x1e:
			sp_register.sp_semaphore_reg = 1;
		break;
	}
	hword = *((unsigned short*)((unsigned char*)readrspreg[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
}

void read_rsp_regd()
{
	if(*address_low == 0x18)
		sp_register.sp_semaphore_reg = 1;
	dword = ((unsigned long long int)(*readrspreg[*address_low])<<32) | *readrspreg[*address_low+4];
}

void write_rsp_reg()
{
	switch(*address_low)
	{
		case 0x10:
			sp_register.w_sp_status_reg = word;
			update_SP();
			return;
		break;
		case 0x14:
		case 0x18:
			return;
		break;
	}
	*readrspreg[*address_low] = word;
	switch(*address_low)
	{
		case 0x8:
			dma_sp_write();
		break;
		case 0xc:
			dma_sp_read();
		break;
		case 0x1c:
			sp_register.sp_semaphore_reg = 0;
		break;
	}
}

void write_rsp_regb()
{
	switch(*address_low)
	{
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
			*((unsigned char*)&sp_register.w_sp_status_reg + ((*address_low&3)^S8) ) = byte;
			return;
		break;
		case 0x14:
		case 0x15:
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x1a:
		case 0x1b:
			return;
		break;
	}
	*((unsigned char*)readrspreg[*address_low & 0xfffc] + ((*address_low&3)^S8) ) = byte;
	switch(*address_low)
	{
		case 0x8:
		case 0x9:
		case 0xa:
		case 0xb:
			dma_sp_write();
		break;
		case 0xc:
		case 0xd:
		case 0xe:
		case 0xf:
			dma_sp_read();
		break;
		case 0x1c:
		case 0x1d:
		case 0x1e:
		case 0x1f:
			sp_register.sp_semaphore_reg = 0;
		break;
	}
}

void write_rsp_regh()
{
	switch(*address_low)
	{
		case 0x10:
		case 0x12:
			*((unsigned short*)((unsigned char*)&sp_register.w_sp_status_reg + ((*address_low&3)^S16) )) = hword;
			return;
		break;
		case 0x14:
		case 0x16:
		case 0x18:
		case 0x1a:
			return;
		break;
	}
	*((unsigned short*)((unsigned char*)readrspreg[*address_low & 0xfffc] + ((*address_low&3)^S16) )) = hword;
	switch(*address_low)
	{
		case 0x8:
		case 0xa:
			dma_sp_write();
		break;
		case 0xc:
		case 0xe:
			dma_sp_read();
		break;
		case 0x1c:
		case 0x1e:
			sp_register.sp_semaphore_reg = 0;
		break;
	}
}

void write_rsp_regd()
{
	switch(*address_low)
	{
		case 0x10:
			sp_register.w_sp_status_reg = dword >> 32;
			update_SP();
			return;
		break;
		case 0x18:
			sp_register.sp_semaphore_reg = 0;
			return;
		break;
	}
	*readrspreg[*address_low] = dword >> 32;
	*readrspreg[*address_low] = dword & 0xFFFFFFFF;
	switch(*address_low)
	{
		case 0x8:
			dma_sp_write();
			dma_sp_read();
		break;
	}
}

void read_rsp()
{
	word = *(readrsp[*address_low]);
}

void read_rspb()
{
	byte = *((unsigned char*)readrsp[*address_low & 0xfffc] + ((*address_low&3)^S8) );
}

void read_rsph()
{
	hword = *((unsigned short*)((unsigned char*)readrsp[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
}

void read_rspd()
{
	dword = ((unsigned long long int)(*readrsp[*address_low])<<32) | *readrsp[*address_low+4];
}

void write_rsp()
{
	*readrsp[*address_low] = word;
}

void write_rspb()
{
	*((unsigned char*)readrsp[*address_low & 0xfffc] + ((*address_low&3)^S8) ) = byte;
}

void write_rsph()
{
	*((unsigned short*)((unsigned char*)readrsp[*address_low & 0xfffc] + ((*address_low&3)^S16) )) = hword;
}

void write_rspd()
{
	*readrsp[*address_low] = dword >> 32;
	*readrsp[*address_low+4] = dword & 0xFFFFFFFF;
}

void read_dp()
{
	word = *(readdp[*address_low]);
}

void read_dpb()
{
	byte = *((unsigned char*)readdp[*address_low & 0xfffc] + ((*address_low&3)^S8) );
}

void read_dph()
{
	hword = *((unsigned short*)((unsigned char*)readdp[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
}

void read_dpd()
{
	dword = ((unsigned long long int)(*readdp[*address_low])<<32) | *readdp[*address_low+4];
}

void write_dp()
{
	switch(*address_low)
	{
		case 0xc:
			dpc_register.w_dpc_status = word;
			update_DPC();
			return;
		break;
		case 0x8:
		case 0x10:
		case 0x14:
		case 0x18:
		case 0x1c:
			return;
		break;
	}
	*readdp[*address_low] = word;
	switch(*address_low)
	{
		case 0x0:
			dpc_register.dpc_current = dpc_register.dpc_start;
		break;
		case 0x4:
			processRDPList();
			MI_register.mi_intr_reg |= 0x20;
			check_interupt();
		break;
	}
}

void write_dpb()
{
	switch(*address_low)
	{
		case 0xc:
		case 0xd:
		case 0xe:
		case 0xf:
			*((unsigned char*)&dpc_register.w_dpc_status + ((*address_low&3)^S8) ) = byte;
			update_DPC();
			return;
		break;
		case 0x8:
		case 0x9:
		case 0xa:
		case 0xb:
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
		case 0x14:
		case 0x15:
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x1a:
		case 0x1b:
		case 0x1c:
		case 0x1d:
		case 0x1e:
		case 0x1f:
			return;
		break;
	}
	*((unsigned char*)readdp[*address_low & 0xfffc] + ((*address_low&3)^S8) ) = byte;
	switch(*address_low)
	{
		case 0x0:
		case 0x1:
		case 0x2:
		case 0x3:
			dpc_register.dpc_current = dpc_register.dpc_start;
		break;
		case 0x4:
		case 0x5:
		case 0x6:
		case 0x7:
			processRDPList();
			MI_register.mi_intr_reg |= 0x20;
			check_interupt();
		break;
	}
}

void write_dph()
{
	switch(*address_low)
	{
		case 0xc:
		case 0xe:
			*((unsigned short*)((unsigned char*)&dpc_register.w_dpc_status + ((*address_low&3)^S16) )) = hword;
			update_DPC();
			return;
		break;
		case 0x8:
		case 0xa:
		case 0x10:
		case 0x12:
		case 0x14:
		case 0x16:
		case 0x18:
		case 0x1a:
		case 0x1c:
		case 0x1e:
			return;
		break;
	}
	*((unsigned short*)((unsigned char*)readdp[*address_low & 0xfffc] + ((*address_low&3)^S16) )) = hword;
	switch(*address_low)
	{
		case 0x0:
		case 0x2:
			dpc_register.dpc_current = dpc_register.dpc_start;
		break;
		case 0x4:
		case 0x6:
			processRDPList();
			MI_register.mi_intr_reg |= 0x20;
			check_interupt();
		break;
	}
}

void write_dpd()
{
	switch(*address_low)
	{
		case 0x8:
			dpc_register.w_dpc_status = dword & 0xFFFFFFFF;
			update_DPC();
			return;
		break;
		case 0x10:
		case 0x18:
			return;
		break;
	}
	*readdp[*address_low] = dword >> 32;
	*readdp[*address_low+4] = dword & 0xFFFFFFFF;
	switch(*address_low)
	{
		case 0x0:
			dpc_register.dpc_current = dpc_register.dpc_start;
			processRDPList();
			MI_register.mi_intr_reg |= 0x20;
			check_interupt();
		break;
	}
}

void read_dps()
{
	word = *(readdps[*address_low]);
}

void read_dpsb()
{
	byte = *((unsigned char*)readdps[*address_low & 0xfffc] + ((*address_low&3)^S8) );
}

void read_dpsh()
{
	hword = *((unsigned short*)((unsigned char*)readdps[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
}

void read_dpsd()
{
	dword = ((unsigned long long int)(*readdps[*address_low])<<32) | *readdps[*address_low+4];
}

void write_dps()
{
	*readdps[*address_low] = word;
}

void write_dpsb()
{
	*((unsigned char*)readdps[*address_low & 0xfffc] + ((*address_low&3)^S8) ) = byte;
}

void write_dpsh()
{
	*((unsigned short*)((unsigned char*)readdps[*address_low & 0xfffc] + ((*address_low&3)^S16) )) = hword;
}

void write_dpsd()
{
	*readdps[*address_low] = dword >> 32;
	*readdps[*address_low+4] = dword & 0xFFFFFFFF;
}

void read_mi()
{
	word = *(readmi[*address_low]);
}

void read_mib()
{
	byte = *((unsigned char*)readmi[*address_low & 0xfffc] + ((*address_low&3)^S8) );
}

void read_mih()
{
	hword = *((unsigned short*)((unsigned char*)readmi[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
}

void read_mid()
{
	dword = ((unsigned long long int)(*readmi[*address_low])<<32) | *readmi[*address_low+4];
}

void write_mi()
{
	switch(*address_low)
	{
		case 0x0:
			MI_register.w_mi_init_mode_reg = word;
			update_MI_init_mode_reg();
		break;
		case 0xc:
			MI_register.w_mi_intr_mask_reg = word;
			update_MI_intr_mask_reg();
			check_interupt();
			update_count();
			if (r4300.next_interrupt <= Count) gen_interupt();
		break;
	}
}

void write_mib()
{
	switch(*address_low)
	{
		case 0x0:
		case 0x1:
		case 0x2:
		case 0x3:
			*((unsigned char*)&MI_register.w_mi_init_mode_reg + ((*address_low&3)^S8) ) = byte;
			update_MI_init_mode_reg();
		break;
		case 0xc:
		case 0xd:
		case 0xe:
		case 0xf:
			*((unsigned char*)&MI_register.w_mi_intr_mask_reg + ((*address_low&3)^S8) ) = byte;
			update_MI_intr_mask_reg();
			check_interupt();
			update_count();
			if (r4300.next_interrupt <= Count) gen_interupt();
		break;
	}
}

void write_mih()
{
	switch(*address_low)
	{
		case 0x0:
		case 0x2:
			*((unsigned short*)((unsigned char*)&MI_register.w_mi_init_mode_reg + ((*address_low&3)^S16) )) = hword;
			update_MI_init_mode_reg();
		break;
		case 0xc:
		case 0xe:
			*((unsigned short*)((unsigned char*)&MI_register.w_mi_intr_mask_reg + ((*address_low&3)^S16) )) = hword;
			update_MI_intr_mask_reg();
			check_interupt();
			update_count();
			if (r4300.next_interrupt <= Count) gen_interupt();
		break;
	}
}

void write_mid()
{
	switch(*address_low)
	{
		case 0x0:
			MI_register.w_mi_init_mode_reg = dword >> 32;
			update_MI_init_mode_reg();
		break;
		case 0x8:
			MI_register.w_mi_intr_mask_reg = dword & 0xFFFFFFFF;
			update_MI_intr_mask_reg();
			check_interupt();
			update_count();
			if (r4300.next_interrupt <= Count) gen_interupt();
		break;
	}
}

void read_vi()
{
	switch(*address_low)
	{
		case 0x10:
			update_count();
			vi_register.vi_current = (vi_register.vi_delay-(r4300.next_vi-Count))/1500;
			vi_register.vi_current = (vi_register.vi_current%vi_register.vi_v_sync);
			vi_register.vi_current = (vi_register.vi_current&(~1))|r4300.vi_field;
		break;
	}
	word = *(readvi[*address_low]);
}

void read_vib()
{
	switch(*address_low)
	{
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
			update_count();
			vi_register.vi_current = (vi_register.vi_delay-(r4300.next_vi-Count))/1500;
			vi_register.vi_current = (vi_register.vi_current%vi_register.vi_v_sync);
			vi_register.vi_current = (vi_register.vi_current&(~1))|r4300.vi_field;
		break;
	}
	byte = *((unsigned char*)readvi[*address_low & 0xfffc]+ ((*address_low&3)^S8) );
}

void read_vih()
{
	switch(*address_low)
	{
		case 0x10:
		case 0x12:
			update_count();
			vi_register.vi_current = (vi_register.vi_delay-(r4300.next_vi-Count))/1500;
			vi_register.vi_current = (vi_register.vi_current%vi_register.vi_v_sync);
			vi_register.vi_current = (vi_register.vi_current&(~1))|r4300.vi_field;
		break;
	}
	hword = *((unsigned short*)((unsigned char*)readvi[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
}

void read_vid()
{
	switch(*address_low)
	{
		case 0x10:
			update_count();
			vi_register.vi_current = (vi_register.vi_delay-(r4300.next_vi-Count))/1500;
			vi_register.vi_current = (vi_register.vi_current%vi_register.vi_v_sync);
			vi_register.vi_current = (vi_register.vi_current&(~1))|r4300.vi_field;
		break;
	}
	dword = ((unsigned long long int)(*readvi[*address_low])<<32) | *readvi[*address_low+4];
}

void write_vi()
{
	switch(*address_low)
	{
		case 0x0:
			if (vi_register.vi_status != word)
			{
				vi_register.vi_status = word;
				viStatusChanged();
			}
			return;
		break;
		case 0x8:
			if (vi_register.vi_width != word)
			{
				vi_register.vi_width = word;
				viWidthChanged();
			}
			return;
		break;
		case 0x10:
			MI_register.mi_intr_reg &= 0xFFFFFFF7;
			check_interupt();
			return;
		break;
	}
	*readvi[*address_low] = word;
}

void write_vib()
{
	int temp;
	switch(*address_low)
	{
		case 0x0:
		case 0x1:
		case 0x2:
		case 0x3:
			temp = vi_register.vi_status;
			*((unsigned char*)&temp + ((*address_low&3)^S8) ) = byte;
			if (vi_register.vi_status != temp)
			{
				vi_register.vi_status = temp;
				viStatusChanged();
			}
			return;
		break;
		case 0x8:
		case 0x9:
		case 0xa:
		case 0xb:
			temp = vi_register.vi_status;
			*((unsigned char*)&temp + ((*address_low&3)^S8) ) = byte;
			if (vi_register.vi_width != temp)
			{
				vi_register.vi_width = temp;
				viWidthChanged();
			}
			return;
		break;
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
			MI_register.mi_intr_reg &= 0xFFFFFFF7;
			check_interupt();
			return;
		break;
	}
	*((unsigned char*)readvi[*address_low & 0xfffc] + ((*address_low&3)^S8) ) = byte;
}

void write_vih()
{
	int temp;
	switch(*address_low)
	{
		case 0x0:
		case 0x2:
			temp = vi_register.vi_status;
			*((unsigned short*)((unsigned char*)&temp + ((*address_low&3)^S16) )) = hword;
			if (vi_register.vi_status != temp)
			{
				vi_register.vi_status = temp;
				viStatusChanged();
			}
			return;
		break;
		case 0x8:
		case 0xa:
			temp = vi_register.vi_status;
			*((unsigned short*)((unsigned char*)&temp + ((*address_low&3)^S16) )) = hword;
			if (vi_register.vi_width != temp)
			{
				vi_register.vi_width = temp;
				viWidthChanged();
			}
			return;
		break;
		case 0x10:
		case 0x12:
			MI_register.mi_intr_reg &= 0xFFFFFFF7;
			check_interupt();
			return;
		break;
	}
	*((unsigned short*)((unsigned char*)readvi[*address_low & 0xfffc] + ((*address_low&3)^S16) )) = hword;
}

void write_vid()
{
	switch(*address_low)
	{
		case 0x0:
			if (vi_register.vi_status != dword >> 32)
			{
				vi_register.vi_status = dword >> 32;
				viStatusChanged();
			}
			vi_register.vi_origin = dword & 0xFFFFFFFF;
			return;
		break;
		case 0x8:
			if (vi_register.vi_width != dword >> 32)
			{
				vi_register.vi_width = dword >> 32;
				viWidthChanged();
			}
			vi_register.vi_v_intr = dword & 0xFFFFFFFF;
			return;
		break;
		case 0x10:
			MI_register.mi_intr_reg &= 0xFFFFFFF7;
			check_interupt();
			vi_register.vi_burst = dword & 0xFFFFFFFF;
			return;
		break;
	}
	*readvi[*address_low] = dword >> 32;
	*readvi[*address_low+4] = dword & 0xFFFFFFFF;
}

void read_ai()
{
	switch(*address_low)
	{
		case 0x4:
			update_count();
			if (ai_register.current_delay != 0 && get_event(AI_INT) != 0 && (get_event(AI_INT)-Count) < 0x80000000)
				word = ((get_event(AI_INT)-Count)*(long long)ai_register.current_len)/ai_register.current_delay;
			else
				word = 0;
		break;
	}
	word = *(readai[*address_low]);
}

void read_aib()
{
	unsigned long len;
	switch(*address_low)
	{
		case 0x4:
		case 0x5:
		case 0x6:
		case 0x7:
			update_count();
			if (ai_register.current_delay != 0 && get_event(AI_INT) != 0)
				len = ((get_event(AI_INT)-Count)*(long long)ai_register.current_len)/ai_register.current_delay;
			else
				len = 0;
			byte = *((unsigned char*)&len + ((*address_low&3)^S8) );
		break;
	}
	byte = *((unsigned char*)readai[*address_low & 0xfffc] + ((*address_low&3)^S8) );
}

void read_aih()
{
	unsigned long len;
	switch(*address_low)
	{
		case 0x4:
		case 0x6:
			update_count();
			if (ai_register.current_delay != 0 && get_event(AI_INT) != 0)
				len = ((get_event(AI_INT)-Count)*(long long)ai_register.current_len)/ai_register.current_delay;
			else
				len = 0;
			hword = *((unsigned short*)((unsigned char*)&len + ((*address_low&3)^S16) ));
		break;
	}
	hword = *((unsigned short*)((unsigned char*)readai[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
}

void read_aid()
{
	switch(*address_low)
	{
		case 0x0:
			update_count();
			if (ai_register.current_delay != 0 && get_event(AI_INT) != 0)
				dword = ((get_event(AI_INT)-Count)*(long long)ai_register.current_len)/ai_register.current_delay;
			else
				dword = (unsigned long long)ai_register.ai_dram_addr << 32 & 0xFFFFFFFF00000000LL;
		break;
	}
	dword = ((unsigned long long int)(*readai[*address_low])<<32) | *readai[*address_low+4];
}

void write_ai()
{
	unsigned long delay=0;
	switch(*address_low)
	{
		case 0x4:
			ai_register.ai_len = word;
			aiLenChanged();
			switch(ROM_HEADER.Country_code&0xFF)
			{
				case 0x44:
				case 0x46:
				case 0x49:
				case 0x50:
				case 0x53:
				case 0x55:
				case 0x58:
				case 0x59:
				{
					unsigned long f = 49656530/(ai_register.ai_dacrate+1);
					if (f)
					delay = ((unsigned long long)ai_register.ai_len*vi_register.vi_delay*50)/(f*4);
				}
				break;
				case 0x37:
				case 0x41:
				case 0x45:
				case 0x4a:
				{
					unsigned long f = 48681812/(ai_register.ai_dacrate+1);
					if (f)
					delay = ((unsigned long long)ai_register.ai_len*vi_register.vi_delay*60)/(f*4);
				}
				break;
			}
			if (no_audio_delay) delay = 0;
			if (ai_register.ai_status & 0x40000000) // busy
			{
				ai_register.next_delay = delay;
				ai_register.next_len = ai_register.ai_len;
				ai_register.ai_status |= 0x80000000;
			}
			else
			{
				ai_register.current_delay = delay;
				ai_register.current_len = ai_register.ai_len;
				update_count();
				add_interupt_event(AI_INT, delay);
				ai_register.ai_status |= 0x40000000;
			}
			return;
		break;
		case 0xc:
			MI_register.mi_intr_reg &= 0xFFFFFFFB;
			check_interupt();
			return;
		break;
		case 0x10:
			if (ai_register.ai_dacrate != word)
			{
				ai_register.ai_dacrate = word;
				switch(ROM_HEADER.Country_code&0xFF)
				{
				case 0x44:
				case 0x46:
				case 0x49:
				case 0x50:
				case 0x53:
				case 0x55:
				case 0x58:
				case 0x59:
					aiDacrateChanged(SYSTEM_PAL);
				break;
				case 0x37:
				case 0x41:
				case 0x45:
				case 0x4a:
					aiDacrateChanged(SYSTEM_NTSC);
				break;
				}
			}
			return;
		break;
	}
	*readai[*address_low] = word;
}

void write_aib()
{
	int temp;
	unsigned long delay=0;
	switch(*address_low)
	{
		case 0x4:
		case 0x5:
		case 0x6:
		case 0x7:
			temp = ai_register.ai_len;
			*((unsigned char*)&temp + ((*address_low&3)^S8) ) = byte;
			ai_register.ai_len = temp;
			aiLenChanged();
			switch(ROM_HEADER.Country_code&0xFF)
			{
				case 0x44:
				case 0x46:
				case 0x49:
				case 0x50:
				case 0x53:
				case 0x55:
				case 0x58:
				case 0x59:
					delay = ((unsigned long long)ai_register.ai_len*(ai_register.ai_dacrate+1)*vi_register.vi_delay*50)/49656530;
				break;
				case 0x37:
				case 0x41:
				case 0x45:
				case 0x4a:
					delay = ((unsigned long long)ai_register.ai_len*(ai_register.ai_dacrate+1)*vi_register.vi_delay*60)/48681812;
				break;
			}
			//delay = 0;
			if (ai_register.ai_status & 0x40000000) // busy
			{
				ai_register.next_delay = delay;
				ai_register.next_len = ai_register.ai_len;
				ai_register.ai_status |= 0x80000000;
			}
			else
			{
				ai_register.current_delay = delay;
				ai_register.current_len = ai_register.ai_len;
				update_count();
				add_interupt_event(AI_INT, delay/2);
				ai_register.ai_status |= 0x40000000;
			}
			return;
		break;
		case 0xc:
		case 0xd:
		case 0xe:
		case 0xf:
			MI_register.mi_intr_reg &= 0xFFFFFFFB;
			check_interupt();
			return;
		break;
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
			temp = ai_register.ai_dacrate;
			*((unsigned char*)&temp + ((*address_low&3)^S8) ) = byte;
			if (ai_register.ai_dacrate != temp)
			{
				ai_register.ai_dacrate = temp;
				switch(ROM_HEADER.Country_code&0xFF)
				{
					case 0x44:
					case 0x46:
					case 0x49:
					case 0x50:
					case 0x53:
					case 0x55:
					case 0x58:
					case 0x59:
						aiDacrateChanged(SYSTEM_PAL);
					break;
					case 0x37:
					case 0x41:
					case 0x45:
					case 0x4a:
						aiDacrateChanged(SYSTEM_NTSC);
					break;
				}
			}
			return;
		break;
	}
	*((unsigned char*)readai[*address_low & 0xfffc] + ((*address_low&3)^S8) ) = byte;
}

void write_aih()
{
	int temp;
	unsigned long delay=0;
	switch(*address_low)
	{
		case 0x4:
		case 0x6:
			temp = ai_register.ai_len;
			*((unsigned short*)((unsigned char*)&temp + ((*address_low&3)^S16) )) = hword;
			ai_register.ai_len = temp;
			aiLenChanged();
			switch(ROM_HEADER.Country_code&0xFF)
			{
				case 0x44:
				case 0x46:
				case 0x49:
				case 0x50:
				case 0x53:
				case 0x55:
				case 0x58:
				case 0x59:
					delay = ((unsigned long long)ai_register.ai_len*(ai_register.ai_dacrate+1)*vi_register.vi_delay*50)/49656530;
				break;
				case 0x37:
				case 0x41:
				case 0x45:
				case 0x4a:
					delay = ((unsigned long long)ai_register.ai_len*(ai_register.ai_dacrate+1)*vi_register.vi_delay*60)/48681812;
				break;
			}
			if (no_audio_delay) delay = 0;
			if (ai_register.ai_status & 0x40000000) // busy
			{
				ai_register.next_delay = delay;
				ai_register.next_len = ai_register.ai_len;
				ai_register.ai_status |= 0x80000000;
			}
			else
			{
				ai_register.current_delay = delay;
				ai_register.current_len = ai_register.ai_len;
				update_count();
				add_interupt_event(AI_INT, delay/2);
				ai_register.ai_status |= 0x40000000;
			}
			return;
		break;
		case 0xc:
		case 0xe:
			MI_register.mi_intr_reg &= 0xFFFFFFFB;
			check_interupt();
			return;
		break;
		case 0x10:
		case 0x12:
			temp = ai_register.ai_dacrate;
			*((unsigned short*)((unsigned char*)&temp + ((*address_low&3)^S16) )) = hword;
			if (ai_register.ai_dacrate != temp)
			{
				ai_register.ai_dacrate = temp;
				switch(ROM_HEADER.Country_code&0xFF)
				{
					case 0x44:
					case 0x46:
					case 0x49:
					case 0x50:
					case 0x53:
					case 0x55:
					case 0x58:
					case 0x59:
						aiDacrateChanged(SYSTEM_PAL);
					break;
					case 0x37:
					case 0x41:
					case 0x45:
					case 0x4a:
						aiDacrateChanged(SYSTEM_NTSC);
					break;
				}
			}
			return;
		break;
	}
	*((unsigned short*)((unsigned char*)readai[*address_low & 0xfffc] + ((*address_low&3)^S16) )) = hword;
}

void write_aid()
{
	unsigned long delay=0;
	switch(*address_low)
	{
		case 0x0:
			ai_register.ai_dram_addr = dword >> 32;
			ai_register.ai_len = dword & 0xFFFFFFFF;
			aiLenChanged();
			switch(ROM_HEADER.Country_code&0xFF)
			{
				case 0x44:
				case 0x46:
				case 0x49:
				case 0x50:
				case 0x53:
				case 0x55:
				case 0x58:
				case 0x59:
					delay = ((unsigned long long)ai_register.ai_len*(ai_register.ai_dacrate+1)*
					vi_register.vi_delay*50)/49656530;
				break;
				case 0x37:
				case 0x41:
				case 0x45:
				case 0x4a:
					delay = ((unsigned long long)ai_register.ai_len*(ai_register.ai_dacrate+1)*
					vi_register.vi_delay*60)/48681812;
				break;
			}
			if (no_audio_delay) delay = 0;
			if (ai_register.ai_status & 0x40000000) // busy
			{
				ai_register.next_delay = delay;
				ai_register.next_len = ai_register.ai_len;
				ai_register.ai_status |= 0x80000000;
			}
			else
			{
				ai_register.current_delay = delay;
				ai_register.current_len = ai_register.ai_len;
				update_count();
				add_interupt_event(AI_INT, delay/2);
				ai_register.ai_status |= 0x40000000;
			}
			return;
		break;
		case 0x8:
			ai_register.ai_control = dword >> 32;
			MI_register.mi_intr_reg &= 0xFFFFFFFB;
			check_interupt();
			return;
		break;
		case 0x10:
			if (ai_register.ai_dacrate != dword >> 32)
			{
				ai_register.ai_dacrate = dword >> 32;
				switch(ROM_HEADER.Country_code&0xFF)
				{
					case 0x44:
					case 0x46:
					case 0x49:
					case 0x50:
					case 0x53:
					case 0x55:
					case 0x58:
					case 0x59:
					aiDacrateChanged(SYSTEM_PAL);
					break;
					case 0x37:
					case 0x41:
					case 0x45:
					case 0x4a:
					aiDacrateChanged(SYSTEM_NTSC);
					break;
				}
			}
			ai_register.ai_bitrate = dword & 0xFFFFFFFF;
			return;
		break;
	}
	*readai[*address_low] = dword >> 32;
	*readai[*address_low+4] = dword & 0xFFFFFFFF;
}

void read_pi()
{
	if(*address_low >= sizeof(readpi)) {
		word = trash;
	}
	else {
		word = *(readpi[*address_low]);
	}
}

void read_pib()
{
	if((*address_low & 0xfffc) >= sizeof(readpi)) {
		byte = trash & 0xFF;
	}
	else {
		byte = *((unsigned char*)readpi[*address_low & 0xfffc] + ((*address_low&3)^S8) );
	}
}

void read_pih()
{
	if((*address_low & 0xfffc) >= sizeof(readpi)) {
		hword = trash & 0xFFFF;
	}
	else {
		hword = *((unsigned short*)((unsigned char*)readpi[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
	}
}

void read_pid()
{
	if(*address_low > sizeof(readpi) - sizeof(long long int)) {
		dword = trash;
	}
	else {
		dword = ((unsigned long long int)(*readpi[*address_low])<<32) | *readpi[*address_low+4];
	}
}

void write_pi()
{
	switch(*address_low)
	{
		case 0x8:
			pi_register.pi_rd_len_reg = word;
			dma_pi_read();
			return;
		break;
		case 0xc:
			pi_register.pi_wr_len_reg = word;
			dma_pi_write();
			return ;
		break;
		case 0x10:
			if (word & 2) MI_register.mi_intr_reg &= 0xFFFFFFEF;
			check_interupt();
			return;
		break;
		case 0x14:
		case 0x18:
		case 0x1c:
		case 0x20:
		case 0x24:
		case 0x28:
		case 0x2c:
		case 0x30:
			*readpi[*address_low] = word & 0xFF;
			return;
		break;
	}
	if(*address_low >= sizeof(readpi)) {
		trash = word;
	}
	else {
		*readpi[*address_low] = word;
	}
}

void write_pib()
{
	switch(*address_low)
	{
		case 0x8:
		case 0x9:
		case 0xa:
		case 0xb:
			*((unsigned char*)&pi_register.pi_rd_len_reg + ((*address_low&3)^S8) ) = byte;
			dma_pi_read();
			return;
		break;
		case 0xc:
		case 0xd:
		case 0xe:
		case 0xf:
			*((unsigned char*)&pi_register.pi_wr_len_reg + ((*address_low&3)^S8) ) = byte;
			dma_pi_write();
			return;
		break;
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
			if (byte) MI_register.mi_intr_reg &= 0xFFFFFFEF;
			check_interupt();
			return;
		break;
		case 0x14:
		case 0x15:
		case 0x16:
		case 0x18:
		case 0x19:
		case 0x1a:
		case 0x1c:
		case 0x1d:
		case 0x1e:
		case 0x20:
		case 0x21:
		case 0x22:
		case 0x24:
		case 0x25:
		case 0x26:
		case 0x28:
		case 0x29:
		case 0x2a:
		case 0x2c:
		case 0x2d:
		case 0x2e:
		case 0x30:
		case 0x31:
		case 0x32:
			return;
		break;
	}
	if(*address_low >= sizeof(readpi)) {
		trash = byte;
	}
	else {
		*((unsigned char*)readpi[*address_low & 0xfffc]	+ ((*address_low&3)^S8) ) = byte;
	}
}

void write_pih()
{
	switch(*address_low)
	{
		case 0x8:
		case 0xa:
			*((unsigned short*)((unsigned char*)&pi_register.pi_rd_len_reg + ((*address_low&3)^S16) )) = hword;
			dma_pi_read();
			return;
		break;
		case 0xc:
		case 0xe:
			*((unsigned short*)((unsigned char*)&pi_register.pi_wr_len_reg + ((*address_low&3)^S16) )) = hword;
			dma_pi_write();
			return;
		break;
		case 0x10:
		case 0x12:
			if (hword) MI_register.mi_intr_reg &= 0xFFFFFFEF;
			check_interupt();
			return;
		break;
		case 0x16:
		case 0x1a:
		case 0x1e:
		case 0x22:
		case 0x26:
		case 0x2a:
		case 0x2e:
		case 0x32:
			*((unsigned short*)((unsigned char*)readpi[*address_low & 0xfffc] + ((*address_low&3)^S16) )) = hword & 0xFF;
			return;
		break;
		case 0x14:
		case 0x18:
		case 0x1c:
		case 0x20:
		case 0x24:
		case 0x28:
		case 0x2c:
		case 0x30:
			return;
		break;
	}
	if(*address_low >= sizeof(readpi)) {
		trash = hword;
	}
	else {
		*((unsigned short*)((unsigned char*)readpi[*address_low & 0xfffc] + ((*address_low&3)^S16) )) = hword;
	}
}

void write_pid()
{
	switch(*address_low)
	{
		case 0x8:
			pi_register.pi_rd_len_reg = dword >> 32;
			dma_pi_read();
			pi_register.pi_wr_len_reg = dword & 0xFFFFFFFF;
			dma_pi_write();
			return;
		break;
		case 0x10:
			if (dword) MI_register.mi_intr_reg &= 0xFFFFFFEF;
			check_interupt();
			*readpi[*address_low+4] = dword & 0xFF;
			return;
		break;
		case 0x18:
		case 0x20:
		case 0x28:
		case 0x30:
			*readpi[*address_low] = (dword >> 32) & 0xFF;
			*readpi[*address_low+4] = dword & 0xFF;
			return;
		break;
	}
	if(*address_low > sizeof(readpi) - sizeof(long long int)) {
		trash = dword & 0xFFFFFFFF;
	}
	else {
		*readpi[*address_low] = dword >> 32;
		*readpi[*address_low+4] = dword & 0xFFFFFFFF;
	}
}

void read_ri()
{
	word = *(readri[*address_low]);
}

void read_rib()
{
	byte = *((unsigned char*)readri[*address_low & 0xfffc] + ((*address_low&3)^S8) );
}

void read_rih()
{
	hword = *((unsigned short*)((unsigned char*)readri[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
}

void read_rid()
{
	dword = ((unsigned long long int)(*readri[*address_low])<<32) | *readri[*address_low+4];
}

void write_ri()
{
	*readri[*address_low] = word;
}

void write_rib()
{
	*((unsigned char*)readri[*address_low & 0xfffc] + ((*address_low&3)^S8) ) = byte;
}

void write_rih()
{
	*((unsigned short*)((unsigned char*)readri[*address_low & 0xfffc] + ((*address_low&3)^S16) )) = hword;
}

void write_rid()
{
	*readri[*address_low] = dword >> 32;
	*readri[*address_low+4] = dword & 0xFFFFFFFF;
}

void read_si()
{
	word = *(readsi[*address_low]);
}

void read_sib()
{
	byte = *((unsigned char*)readsi[*address_low & 0xfffc] + ((*address_low&3)^S8) );
}

void read_sih()
{
	hword = *((unsigned short*)((unsigned char*)readsi[*address_low & 0xfffc] + ((*address_low&3)^S16) ));
}

void read_sid()
{
	dword = ((unsigned long long int)(*readsi[*address_low])<<32) | *readsi[*address_low+4];
}

void write_si()
{
	switch(*address_low)
	{
		case 0x0:
			si_register.si_dram_addr = word;
		break;
		case 0x4:
			si_register.si_pif_addr_rd64b = word;
			dma_si_read();
		break;
		case 0x10:
			si_register.si_pif_addr_wr64b = word;
			dma_si_write();
		break;
		case 0x18:
			MI_register.mi_intr_reg &= 0xFFFFFFFD;
			si_register.si_status &= ~0x1000;
			check_interupt();
		break;
	}
}

void write_sib()
{
	switch(*address_low)
	{
		case 0x0:
		case 0x1:
		case 0x2:
		case 0x3:
			*((unsigned char*)&si_register.si_dram_addr + ((*address_low&3)^S8) ) = byte;
		break;
		case 0x4:
		case 0x5:
		case 0x6:
		case 0x7:
			*((unsigned char*)&si_register.si_pif_addr_rd64b + ((*address_low&3)^S8) ) = byte;
			dma_si_read();
		break;
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
			*((unsigned char*)&si_register.si_pif_addr_wr64b + ((*address_low&3)^S8) ) = byte;
			dma_si_write();
		break;
		case 0x18:
		case 0x19:
		case 0x1a:
		case 0x1b:
			MI_register.mi_intr_reg &= 0xFFFFFFFD;
			si_register.si_status &= ~0x1000;
			check_interupt();
		break;
	}
}

void write_sih()
{
	switch(*address_low)
	{
		case 0x0:
		case 0x2:
			*((unsigned short*)((unsigned char*)&si_register.si_dram_addr + ((*address_low&3)^S16) )) = hword;
		break;
		case 0x4:
		case 0x6:
			*((unsigned short*)((unsigned char*)&si_register.si_pif_addr_rd64b + ((*address_low&3)^S16) )) = hword;
			dma_si_read();
		break;
		case 0x10:
		case 0x12:
			*((unsigned short*)((unsigned char*)&si_register.si_pif_addr_wr64b + ((*address_low&3)^S16) )) = hword;
			dma_si_write();
		break;
		case 0x18:
		case 0x1a:
			MI_register.mi_intr_reg &= 0xFFFFFFFD;
			si_register.si_status &= ~0x1000;
			check_interupt();
		break;
	}
}

void write_sid()
{
	switch(*address_low)
	{
		case 0x0:
			si_register.si_dram_addr = dword >> 32;
			si_register.si_pif_addr_rd64b = dword & 0xFFFFFFFF;
			dma_si_read();
		break;
		case 0x10:
			si_register.si_pif_addr_wr64b = dword >> 32;
			dma_si_write();
		break;
		case 0x18:
			MI_register.mi_intr_reg &= 0xFFFFFFFD;
			si_register.si_status &= ~0x1000;
			check_interupt();
		break;
	}
}

void read_flashram_status()
{
	//if (flashRAMInfo.use_flashram != -1 && *address_low == 0)
	//{
		flashRAMInfo.use_flashram = 1;
		word = flashram_status();
	//}
	//else
	//	printf("unknown read in read_flashram_status\n");
}

void read_flashram_statusb()
{
   //printf("read_flashram_statusb\n");
   byte = 0;
}

void read_flashram_statush()
{
   //printf("read_flashram_statush\n");
   hword = 0;
}

void read_flashram_statusd()
{
   //printf("read_flashram_statusd\n");
   dword = 0;
}

void write_flashram_dummy() {}
void write_flashram_dummyb() {}
void write_flashram_dummyh() {}
void write_flashram_dummyd() {}

void write_flashram_command()
{
	//if (flashRAMInfo.use_flashram != -1 && *address_low == 0)
	//{
		flashram_command(word);
		flashRAMInfo.use_flashram = 1;
	//}
	//else
	//	printf("unknown write in write_flashram_command\n");
}

void write_flashram_commandb(){}
void write_flashram_commandh(){}
void write_flashram_commandd(){}

static unsigned long last_write;
static unsigned long rom_written;

void read_rom()
{
	if (rom_written)
	{
		word = last_write;
		rom_written = 0;
	}
	else
		word = sign_extended(*(unsigned long *)ROMCache_pointer(address & 0x3FFFFFF));
}

void read_romb()
{
	byte = sign_extendedb(*(unsigned char *)ROMCache_pointer(address & 0x3FFFFFF));
}

void read_romh()
{
	hword = sign_extendedh(*(unsigned short *)ROMCache_pointer(address & 0x3FFFFFF));
}

void read_romd()
{
	dword = *(unsigned long long *)ROMCache_pointer(address & 0x3FFFFFF);
}

void write_rom()
{
	last_write = word;
	rom_written = 1;
}

void read_pif()
{
	word = sl(*((unsigned long *)(PIF_RAMb + (address & 0x7FF) - 0x7C0)));
}

void read_pifb()
{
	byte = *(PIF_RAMb + ((address & 0x7FF) - 0x7C0));
}

void read_pifh()
{
	hword = (*(PIF_RAMb + ((address & 0x7FF) - 0x7C0)) << 8) | *(PIF_RAMb + (((address+1) & 0x7FF) - 0x7C0));
}

void read_pifd()
{
	dword = ((unsigned long long)sl(*((unsigned long *)(PIF_RAMb + (address & 0x7FF) - 0x7C0))) << 32)|
		sl(*((unsigned long *)(PIF_RAMb + ((address+4) & 0x7FF) - 0x7C0)));
}

void write_pif()
{
	*((unsigned long *)(PIF_RAMb + (address & 0x7FF) - 0x7C0)) = sl(word);
	if ((address & 0x7FF) == 0x7FC)
	{
		if (PIF_RAMb[0x3F] == 0x08)
		{
			PIF_RAMb[0x3F] = 0;
			update_count();
			add_interupt_event(SI_INT, /*0x100*/0x900);
		}
		else
			update_pif_write();
	}
}

void write_pifb()
{
	*(PIF_RAMb + (address & 0x7FF) - 0x7C0) = byte;
	if ((address & 0x7FF) == 0x7FF)
	{
		if (PIF_RAMb[0x3F] == 0x08)
		{
			PIF_RAMb[0x3F] = 0;
			update_count();
			add_interupt_event(SI_INT, /*0x100*/0x900);
		}
		else
			update_pif_write();
	}
}

void write_pifh()
{
	*(PIF_RAMb + (address & 0x7FF) - 0x7C0) = (hword >> 8);
	*(PIF_RAMb + ((address+1) & 0x7FF) - 0x7C0) = (hword & 0xFF);
	if ((address & 0x7FF) == 0x7FE)
	{
		if (PIF_RAMb[0x3F] == 0x08)
		{
			PIF_RAMb[0x3F] = 0;
			update_count();
			add_interupt_event(SI_INT, /*0x100*/0x900);
		}
		else
			update_pif_write();
	}
}

void write_pifd()
{
	*((unsigned long *)(PIF_RAMb + (address & 0x7FF) - 0x7C0)) = sl((unsigned long)(dword >> 32));
	*((unsigned long *)(PIF_RAMb + (address & 0x7FF) - 0x7C0)) = sl((unsigned long)(dword & 0xFFFFFFFF));
	if ((address & 0x7FF) == 0x7F8)
	{
		if (PIF_RAMb[0x3F] == 0x08)
		{
			PIF_RAMb[0x3F] = 0;
			update_count();
			add_interupt_event(SI_INT, /*0x100*/0x900);
		}
		else
			update_pif_write();
	}
}

unsigned long *fast_mem_access(unsigned long address)
{
   /* This code is performance critical, specially on pure interpreter mode.
    * Removing error checking saves some time, but the emulator may crash. */
	if ((address & 0xc0000000) != 0x80000000)
		address = virtual_to_physical_address(address, 2);

	address &= 0x1ffffffc;

	if (address < MEM_SIZE)
		return (unsigned long*)(rdramb + address);
	else if (address >= 0x10000000)
		return (unsigned long*)ROMCache_pointer(address - 0x10000000);
	else if ((address & 0xffffe000) == 0x04000000)
		return (unsigned long*)(SP_DMEMb + (address & 0x1ffc));
	else
		return NULL;
}
