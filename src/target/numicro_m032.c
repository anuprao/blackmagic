/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2015  Anup Jayapal Rao <anup.kadam@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements the Nuvoton M032xxxxx target specific functions for detecting
 * the device and Flash memory programming.
 *
 * References:
 *
 *   numicro.c from OpenOCD Code authored by following:
 *
 *   Copyright (C) 2011 by James K. Larson
 *   jlarson@pacifier.com
 *
 *   Copyright (C) 2013 Cosmin Gorgovan
 *   cosmin [at] linux-geek [dot] org
 *
 *   Copyright (C) 2014 Pawel Si
 *   stawel+openocd@gmail.com
 *
 *   Copyright (C) 2015 Nemui Trinomius
 *   nemuisan_kawausogasuki@live.jp
 *
 *   Copyright (C) 2017 Zale Yu
 *   CYYU@nuvoton.com
 */

#include "general.h"
#include "target.h"
#include "target_internal.h"
#include "cortexm.h"

//----------------------------------------------------------------------------------------------------

// Nuvoton NuMicro register locations 

#define NUMICRO_APROM_BASE 				0x00000000UL
#define NUMICRO_DATA_BASE 				0x0001F000UL
#define NUMICRO_LDROM_BASE 				0x00100000UL
#define NUMICRO_SPROM_BASE 				0x00200000UL
#define NUMICRO_SPROM_BASE2 			0x00240000UL
#define NUMICRO_SPROM_BASE3 			0x00280000UL
#define NUMICRO_CONFIG_BASE 			0x00300000UL
#define NUMICRO_DATA_DFMC_BASE 			0x00400000UL
#define NUMICRO_SPECIAL_FLASH_OFFSET 	0x0F000000UL

#define NUMICRO_CONFIG0 				(NUMICRO_CONFIG_BASE )		
#define NUMICRO_CONFIG1 				(NUMICRO_CONFIG_BASE + 4)	
#define NUMICRO_CONFIG2 				(NUMICRO_CONFIG_BASE + 8)	

#define NUMICRO_SYSCLK_AHBCLK 			0x40000204UL

#define NUMICRO_FLASH_BASE				0x4000C000UL
#define NUMICRO_FLASH_ISPCON			0x4000C000UL
#define NUMICRO_FLASH_ISPADR			0x4000C004UL
#define NUMICRO_FLASH_ISPDAT			0x4000C008UL
#define NUMICRO_FLASH_ISPCMD			0x4000C00CUL
#define NUMICRO_FLASH_ISPTRG			0x4000C010UL
#define NUMICRO_FLASH_CHEAT 			0x4000C01CUL // Undocumented isp register(may be cheat register) 

/* Command register bits */
#define PWRCON_OSC22M (1 << 2)
#define PWRCON_XTL12M (1 << 0)

#define IPRSTC1_CPU_RST (1 << 1)
#define IPRSTC1_CHIP_RST (1 << 0)

#define AHBCLK_ISP_EN (1 << 2)
#define AHBCLK_SRAM_EN (1 << 4)
#define AHBCLK_TICK_EN (1 << 5)

#define ISPCON_ISPEN (1 << 0)
#define ISPCON_BS_AP (0 << 1)
#define ISPCON_BS_LP (1 << 1)
#define ISPCON_BS_MASK (1 << 1)
#define ISPCON_SPUEN (1 << 2)
#define ISPCON_APUEN (1 << 3)
#define ISPCON_CFGUEN (1 << 4)
#define ISPCON_LDUEN (1 << 5)
#define ISPCON_ISPFF (1 << 6)
#define ISPCON_INTEN (1 << 24)

#define CONFIG0_LOCK_MASK (1 << 1)

#define DHCSR_S_SDE (1 << 20)

/* isp commands */
#define FMC_ISPCMD_READ				0x00U
#define FMC_ISPCMD_WRITE			0x21U
#define FMC_ISPCMD_ERASE			0x22U
#define FMC_ISPCMD_CHIPERASE		0x26U // Undocumented isp "Chip-Erase" command 
#define FMC_ISPCMD_READ_CID			0x0BU
#define FMC_ISPCMD_READ_UID			0x04U
#define FMC_ISPCMD_VECMAP			0x2EU

#define ISPTRG_ISPGO			(1 << 0)

/* access unlock keys */
#define REG_KEY1 0x59U
#define REG_KEY2 0x16U
#define REG_KEY3 0x88U
#define REG_LOCK 0x00U

#define NUMICRO_APROM_SIZE 0x10000
#define NUMICRO_LDROM_SIZE 0x800
#define NUMICRO_SPROM_SIZE 0x200

/* flash page size */
#define NUMICRO_PAGESIZE 512

#define NUMICRO_DFMC_PAGESIZE 256

/* flash MAX banks */
#define NUMICRO_MAX_FLASH_BANKS 4

/* flash mask */
#define NUMICRO_TZ_MASK 0xEFFFFFFFUL
#define NUMICRO_SPROM_MASK 0x00000001UL
#define NUMICRO_SPROM_MINI57_MASK 0x00000002UL
#define NUMICRO_FLASH_OFFSET_MASK 0x00000004UL
#define NUMICRO_SPROM_ISPDAT 0x55AA03UL

/* SPIM flash start address */
#define NUMICRO_SPIM_FLASH_START_ADDRESS 0x8000000UL

//----------------------------------------------------------------------------------------------------

#define NUMICRO_CHIP_ID_ADDRESS 0x40000000UL

#define NUMICRO_SYS_REGLCTL 0x40000100UL

//----------------------------------------------------------------------------------------------------

static bool nu_m032_reg_unlock(target *t)
{
	// bool bRet = false;
	uint32_t uRead = 0xFFFFFFFFUL;

	//DEBUG_INFO("NUMICRO_M032: Unlocking registers ... \n");

	target_mem_write32(t, NUMICRO_SYS_REGLCTL, 0x59);
	target_mem_write32(t, NUMICRO_SYS_REGLCTL, 0x16);
	target_mem_write32(t, NUMICRO_SYS_REGLCTL, 0x88);

	uRead = target_mem_read32(t, NUMICRO_SYS_REGLCTL);
	if (uRead == 0)
	{
		DEBUG_INFO("NUMICRO_M032: Registers not unlocked !");
	}
	else
	{
		DEBUG_INFO("NUMICRO_M032: Registers unlocked !");
	}

	return true;
}

/*
static bool nu_m032_reg_lock(target *t)
{
	//bool bRet = false;

	DEBUG_INFO("NUMICRO_M032: Locking registers ... \n");

	target_mem_write32(t, NUMICRO_SYS_REGLCTL, 0x0);

	DEBUG_INFO("NUMICRO_M032: Registers locked !");

	return true;
}
*/

static void nu_m032_fmc_cmd(target *t, uint32_t cmd, uint32_t addr, uint32_t wdata, uint32_t *rdata)
{
	uint32_t timeout, status;

	target_mem_write32(t, NUMICRO_FLASH_ISPCMD, cmd);

	target_mem_write32(t, NUMICRO_FLASH_ISPADR, addr);

	if(cmd == FMC_ISPCMD_WRITE)
	{
		target_mem_write32(t, NUMICRO_FLASH_ISPDAT, wdata);
	}

	target_mem_write32(t, NUMICRO_FLASH_ISPTRG, ISPTRG_ISPGO);

	/* Wait for busy to clear - check the GO flag */
	timeout = 100;
	for (;;)
	{
		status = target_mem_read32(t, NUMICRO_FLASH_ISPTRG);

		//DEBUG_INFO("Status : 0x%08X\n", status);

		if ((status & (ISPTRG_ISPGO)) == 0)
		{
			//DEBUG_INFO("ISPGO set to 0\n");
			break;
		}

		if (timeout-- <= 0)
		{
			//DEBUG_INFO("Timed out waiting for flash\n");
			return;
		}

		platform_delay(1); // can use busy sleep for short times.
	}

	status = target_mem_read32(t, NUMICRO_FLASH_ISPCON);
	//DEBUG_INFO("ISPCON = 0x%08X\n", status);
	if (status & ISPCON_ISPFF)
	{
		target_mem_write32(t, NUMICRO_FLASH_ISPCON, status);
		//DEBUG_INFO("ISPCON <-- 0x%08X\n", status);
	}

	if(cmd == FMC_ISPCMD_READ)
	{
		*rdata = 0xABCDABCD;
		*rdata = target_mem_read32(t, NUMICRO_FLASH_ISPDAT);
		//DEBUG_INFO("*rdata : 0x%08X\n", *rdata);
	}
}

static int nu_m032_init_isp(target *t, uint32_t uExtraconf)
{
	int retval = 0;
	uint32_t reg_stat;

	// if (target->state != TARGET_HALTED)
	// {
	//	LOG_ERROR("Target not halted");
	//	return -1;
	// }

	{
		retval = nu_m032_reg_unlock(t);
		if (retval != true)
			return -1;

		//DEBUG_INFO("Setting AHBCLK and Enabling ISP\n");

		// Enable ISP/SRAM/TICK Clock 

		// CLK_AHBCLK

		reg_stat = 0x00000000;

		reg_stat = target_mem_read32(t, NUMICRO_SYSCLK_AHBCLK );
		
		//DEBUG_INFO("AHBCLK reg_stat before: 0x%08X\n", reg_stat);

		reg_stat |= AHBCLK_ISP_EN ;
		
		//reg_stat = 0x0;

		target_mem_write32(t, NUMICRO_SYSCLK_AHBCLK , reg_stat);

		//DEBUG_INFO("AHBCLK reg_stat after: 0x%08X\n", reg_stat);

		platform_delay(100);

		// Enable ISP

		reg_stat = 0x00000000;

		reg_stat = target_mem_read32(t, NUMICRO_FLASH_ISPCON );
		
		//DEBUG_INFO("ISPCON reg_stat before : 0x%08X\n", reg_stat);

		//reg_stat |= ISPCON_ISPFF | ISPCON_CFGUEN | ISPCON_ISPEN;
		reg_stat |= ISPCON_ISPFF | ISPCON_ISPEN | uExtraconf;

		//reg_stat = 0x0;

		target_mem_write32(t, NUMICRO_FLASH_ISPCON , reg_stat);

		//DEBUG_INFO("ISPCON reg_stat after : 0x%08X\n", reg_stat);

		platform_delay(100);
	}

	DEBUG_INFO("nu_m032_init_isp is done !\n");

	return 0;
}

static int nu_m032_flash_erase(struct target_flash *f, target_addr addr, size_t len)
{
	int retval;

	uint32_t uRegSample = 0x00000000UL;

	target *t;

	//DEBUG_INFO("In nu_m032_flash_erase ...\n");

	t = f->t;

	retval = nu_m032_init_isp(t, ISPCON_APUEN | ISPCON_LDUEN);
	if (retval != 0)
	{
		return false;
	}

	//DEBUG_INFO("Starting the erase of flash ...\n");
	{
		uint32_t uAddr = addr;
		size_t szSize = len;

		while(szSize > 0)
		{
			nu_m032_fmc_cmd(t, FMC_ISPCMD_ERASE, uAddr, 0, &uRegSample);
			uAddr = uAddr + NUMICRO_PAGESIZE;

			szSize = szSize - NUMICRO_PAGESIZE;

			platform_delay(100);
		}
	}

	//DEBUG_INFO("Erasing Flash done...\n");

	return 0;
}

static int nu_m032_flash_write(struct target_flash *f, target_addr dest, const void *src, size_t len)
{
	uint32_t uRegSample;
	uint32_t uRegWrite;
	uint32_t* pSrc;

	target *t;

	//DEBUG_INFO("In nu_m032_flash_write ...\n");

	t = f->t;
	pSrc = (uint32_t*)src;

	//DEBUG_INFO("Staring writing of flash ...\n");
	{
		uint32_t uAddr = dest;
		size_t szSize = len;

		while(szSize > 0)
		{
			uRegWrite = *pSrc;
			nu_m032_fmc_cmd(t, FMC_ISPCMD_WRITE, uAddr, uRegWrite, &uRegSample);
			uAddr = uAddr + 4;

			pSrc++;

			szSize = szSize - 4;

			//platform_delay(100);
			platform_delay(10);
		}
	}

	//DEBUG_INFO("Writing Flash done...\n");

	return 0;
}

//----------------------------------------------------------------------------------------------------

static void nu_m032_add_flash(target *t, uint32_t addr, size_t length, size_t erasesize)
{
	struct target_flash *f = calloc(1, sizeof(*f));
	if (!f)
	{
		// calloc failed: heap exhaustion
		DEBUG_WARN("calloc: failed in %s\n", __func__);
		return;
	}

	f->start = addr;
	f->length = length;
	f->blocksize = erasesize;
	f->erase = nu_m032_flash_erase;
	f->write = nu_m032_flash_write;
	f->buf_size = erasesize;
	f->erased = 0xff;
	target_add_flash(t, f);
}

static bool nu_m032_cmd_erase_aprom(target *t, int argc, const char **argv)
{
	int retval;
	uint32_t uRegSample = 0x00000000UL;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("In nu_m032_cmd_erase_aprom ...\n");

	retval = nu_m032_init_isp(t, ISPCON_APUEN);
	if (retval != 0)
	{
		return false;
	}

	//

	{
		uint32_t uAddr = NUMICRO_APROM_BASE;
		size_t szSize = NUMICRO_APROM_SIZE;

		while(szSize > 0)
		{
			nu_m032_fmc_cmd(t, FMC_ISPCMD_ERASE, uAddr, 0, &uRegSample);
			uAddr = uAddr + NUMICRO_PAGESIZE;

			DEBUG_INFO("FMC_ISPCMD_ERASE : 0x%08X \n", uAddr);

			szSize = szSize - NUMICRO_PAGESIZE;

			platform_delay(100);
		}
	}

	DEBUG_INFO("Erasing APROM done ... \n");

	return true;
}

static bool nu_m032_cmd_erase_ldrom(target *t, int argc, const char **argv)
{
	int retval;
	uint32_t uRegSample = 0x00000000UL;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("In nu_m032_cmd_erase_ldrom ...\n");

	retval = nu_m032_init_isp(t, ISPCON_LDUEN);
	if (retval != 0)
	{
		return false;
	}

	//

	{
		uint32_t uAddr = NUMICRO_LDROM_BASE;
		size_t szSize = NUMICRO_LDROM_SIZE;

		while(szSize > 0)
		{
			nu_m032_fmc_cmd(t, FMC_ISPCMD_ERASE, uAddr, 0, &uRegSample);
			uAddr = uAddr + NUMICRO_PAGESIZE;

			DEBUG_INFO("FMC_ISPCMD_ERASE : 0x%08X \n", uAddr);

			szSize = szSize - NUMICRO_PAGESIZE;

			platform_delay(100);
		}
	}

	DEBUG_INFO("Erasing LDROM done ... \n");

	return true;
}

static bool nu_m032_cmd_erase_sprom(target *t, int argc, const char **argv)
{
	int retval;
	uint32_t uRegSample = 0x00000000UL;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("In nu_m032_cmd_erase_sprom ...\n");

	retval = nu_m032_init_isp(t, ISPCON_SPUEN);
	if (retval != 0)
	{
		return false;
	}

	//

	{
		uint32_t uAddr = NUMICRO_SPROM_BASE;
		size_t szSize = NUMICRO_SPROM_SIZE;

		while(szSize > 0)
		{
			nu_m032_fmc_cmd(t, FMC_ISPCMD_ERASE, uAddr, 0, &uRegSample);
			uAddr = uAddr + NUMICRO_PAGESIZE;

			DEBUG_INFO("FMC_ISPCMD_ERASE : 0x%08X \n", uAddr);

			szSize = szSize - NUMICRO_PAGESIZE;

			platform_delay(100);
		}
	}

	DEBUG_INFO("Erasing SPROM done ... \n");

	return true;
}

static bool nu_m032_cmd_erase_mass(target *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	//DEBUG_INFO("In nu_m032_cmd_erase_mass ...\n");

	nu_m032_cmd_erase_aprom(t, argc, argv);
	nu_m032_cmd_erase_ldrom(t, argc, argv);
	//nu_m032_cmd_erase_sprom(t, argc, argv);

	return true;
}

static bool nu_m032_cmd_erase_chip(target *t, int argc, const char **argv)
{
	int retval;

	uint32_t uRegSample;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("In nu_m032_cmd_erase_chip ...\n");

	retval = nu_m032_init_isp(t, ISPCON_APUEN | ISPCON_LDUEN | ISPCON_SPUEN );
	if (retval != 0)
	{
		return false;
	}

	//

    //nu_m032_fmc_cmd(t, FMC_ISPCMD_WRITE, Config0, 0, &uRegSample);

    nu_m032_fmc_cmd(t, FMC_ISPCMD_CHIPERASE, 0x0, 0x0, &uRegSample);

	platform_delay(100);

	DEBUG_INFO("Erasing Chip done ... \n");

	return true;
}

static bool nu_m032_set_config0(target *t, int argc, const char **argv)
{
	uint32_t uRegValue = 0x00000000UL;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("Set nu_m032_set_config0 : 0x%08X\n", uRegValue);

	//

	//

	return true;
}

static bool nu_m032_set_config1(target *t, int argc, const char **argv)
{
	uint32_t uRegValue = 0x00000000UL;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("Set nu_m032_set_config1 : 0x%08X\n", uRegValue);

	//

	//

	return true;
}

static bool nu_m032_set_config2(target *t, int argc, const char **argv)
{
	uint32_t uRegValue = 0x00000000UL;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("Set nu_m032_set_config2 : 0x%08X\n", uRegValue);

	//

	//

	return true;
}

static bool nu_m032_read_configs(target *t, int argc, const char **argv)
{
	int retval;

	uint32_t uRegSample;

	uint32_t uRegValue0 = 0xABCDABCD;
	uint32_t uRegValue1 = 0xABCDABCD;
	uint32_t uRegValue2 = 0xABCDABCD;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("In nu_m032_read_configs ...\n");

	retval = nu_m032_init_isp(t, 0x0UL);
	if (retval != 0)
	{
		return false;
	}

	//nu_m032_fmc_cmd(t, FMC_ISPCMD_ERASE, NUMICRO_CONFIG0, 0, &uRegSample);

	//nu_m032_fmc_cmd(t, FMC_ISPCMD_WRITE, NUMICRO_CONFIG0, 0xFFFFFF3F, &uRegValue0);
	//DEBUG_INFO("After write : uRegValue0 : 0x%08X\n", uRegValue0);
	//nu_m032_fmc_cmd(t, FMC_ISPCMD_WRITE, NUMICRO_CONFIG1, 0xFFFFFFFF, &uRegValue1);
	//DEBUG_INFO("After write : uRegValue1 : 0x%08X\n", uRegValue1);

	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ, NUMICRO_CONFIG0, 0, &uRegValue0);
	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ, NUMICRO_CONFIG1, 0, &uRegValue1);	
	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ, NUMICRO_CONFIG2, 0, &uRegValue2);	

	DEBUG_INFO("Read Config0 : 0x%08X\n", uRegValue0);
	DEBUG_INFO("Read Config1 : 0x%08X\n", uRegValue1);
	DEBUG_INFO("Read Config2 : 0x%08X\n", uRegValue2);

	//

	if ((uRegValue0 & (1<<7)) == 0)
	{
		DEBUG_INFO("CBS=0: Boot From LDROM\n");
	}
	else
	{
		DEBUG_INFO("CBS=1: Boot From APROM\n");
	}

	if ((uRegValue0 & CONFIG0_LOCK_MASK) == 0) 
	{
		DEBUG_INFO("Flash is secure locked!\n");
		DEBUG_INFO("TO UNLOCK FLASH,EXECUTE chip_erase COMMAND!!\n");
	}
	else 
	{
		DEBUG_INFO("Flash is not locked!\n");
	}

	//

	uRegSample = target_mem_read32(t, NUMICRO_FLASH_ISPCON);
	//DEBUG_INFO("Read uRegSample : 0x%08X\n", uRegSample);
	if ((uRegSample & ISPCON_BS_MASK) == 0)
	{
		DEBUG_INFO("ISPCTL reports: Boot From APROM\n");
	}
	else
	{
		DEBUG_INFO("ISPCTL reports: Boot From LDROM\n");
	}

	return true;
}

static bool nu_m032_read_uid(target *t, int argc, const char **argv)
{
	int retval;

	uint32_t uRegSample;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("In nu_m032_read_uid ...\n");

	retval = nu_m032_init_isp(t, 0x0UL);
	if (retval != 0)
	{
		return false;
	}

	//

	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ_UID, 0x00U, 0, &uRegSample);
	DEBUG_INFO("Read FMC_ISPCMD_READ_UID-0 : 0x%08X\n", uRegSample);

	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ_UID, 0x04U, 0, &uRegSample);
	DEBUG_INFO("Read FMC_ISPCMD_READ_UID-1 : 0x%08X\n", uRegSample);

	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ_UID, 0x08U, 0, &uRegSample);
	DEBUG_INFO("Read FMC_ISPCMD_READ_UID-2 : 0x%08X\n", uRegSample);

	return true;
}

static bool nu_m032_read_cid(target *t, int argc, const char **argv)
{
	int retval;

	uint32_t uRegSample;

	(void)argc;
	(void)argv;

	DEBUG_INFO("In nu_m032_read_cid ...\n");

	retval = nu_m032_init_isp(t, 0x0UL);
	if (retval != 0)
	{
		return false;
	}

	//

	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ_CID, 0x00U, 0, &uRegSample);
	DEBUG_INFO("Read FMC_ISPCMD_READ_CID-0 : 0x%08X\n", uRegSample);

	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ_CID, 0x04U, 0, &uRegSample);
	DEBUG_INFO("Read FMC_ISPCMD_READ_CID-1 : 0x%08X\n", uRegSample);

	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ_CID, 0x08U, 0, &uRegSample);
	DEBUG_INFO("Read FMC_ISPCMD_READ_CID-2 : 0x%08X\n", uRegSample);
	
	nu_m032_fmc_cmd(t, FMC_ISPCMD_READ_CID, 0x0CU, 0, &uRegSample);
	DEBUG_INFO("Read FMC_ISPCMD_READ_CID-3 : 0x%08X\n", uRegSample);

	return true;
}

static bool nu_m032_read_aprom_page1(target *t, int argc, const char **argv)
{
	int retval;

	uint32_t uRegSample;

	(void)argc;
	(void)argv;

	//DEBUG_INFO("In nu_m032_read_aprom_page1 ...\n");
	
	retval = nu_m032_init_isp(t, 0x0UL);
	if (retval != 0)
	{
		return false;
	}

	DEBUG_INFO("Reading APROM 1st page only ...\n");
	{
		uint32_t uAddr = NUMICRO_APROM_BASE;
		size_t szSize = NUMICRO_PAGESIZE;

		while(szSize > 0)
		{
			uRegSample = 0x00000000UL;

			nu_m032_fmc_cmd(t, FMC_ISPCMD_READ, uAddr, 0, &uRegSample);

			uAddr = uAddr + 4;

			DEBUG_INFO("0x%08X ", uRegSample);
			if((uAddr & 0x0F) == 0x0F)
			{
				DEBUG_INFO("\n");
			}

			szSize = szSize - 4;
		}
	}

	DEBUG_INFO("\n");

	DEBUG_INFO("Reading APROM 2nd page only ...\n");
	{
		uint32_t uAddr = NUMICRO_APROM_BASE + NUMICRO_PAGESIZE;
		size_t szSize = NUMICRO_PAGESIZE;

		while(szSize > 0)
		{
			uRegSample = 0x00000000UL;

			nu_m032_fmc_cmd(t, FMC_ISPCMD_READ, uAddr, 0, &uRegSample);

			uAddr = uAddr + 4;

			DEBUG_INFO("0x%08X ", uRegSample);
			if((uAddr & 0x0F) == 0x0F)
			{
				DEBUG_INFO("\n");
			}

			szSize = szSize - 4;
		}
	}

	DEBUG_INFO("\n");

	return true;
}

const struct command_s nu_m032_cmd_list[] = {
	{"erase_aprom", (cmd_handler)nu_m032_cmd_erase_aprom, "Erase APROM"},
	{"erase_ldrom", (cmd_handler)nu_m032_cmd_erase_ldrom, "Erase LDROM"},
	{"erase_sprom", (cmd_handler)nu_m032_cmd_erase_sprom, "Erase SPROM"},
	{"erase_mass", (cmd_handler)nu_m032_cmd_erase_mass, "Erase APROM, LDROM and SPROM"},
	{"erase_chip", (cmd_handler)nu_m032_cmd_erase_chip, "Erase chip via undocumented command"},
	{"set_config0", (cmd_handler)nu_m032_set_config0, "Set CONFIG0 Register"},
	{"set_config1", (cmd_handler)nu_m032_set_config1, "Set CONFIG1 Register"},
	{"set_config2", (cmd_handler)nu_m032_set_config2, "Set CONFIG2 Register"},
	{"read_configs", (cmd_handler)nu_m032_read_configs, "Read CONFIG Registers"},
	{"read_uid", (cmd_handler)nu_m032_read_uid, "Read UID"},
	{"read_cid", (cmd_handler)nu_m032_read_cid, "Read CID"},
	{"read_aprom_page1", (cmd_handler)nu_m032_read_aprom_page1, "nu_m032_read_aprom_page1"},
	{NULL, NULL, NULL}};

/**
	\brief identify the m032 chip
*/
bool nu_m032_probe(target *t)
{
	//bool bCheck;

	uint32_t uChipID = 0x00000000UL;

	uint16_t stored_idcode = t->idcode;

	size_t ram_size_INTERNAL;

	size_t flash_size_APROM;
	size_t block_size_APROM;

	// size_t flash_size_DATA;
	// size_t block_size_DATA;

	size_t flash_size_LDROM;
	size_t block_size_LDROM;

	size_t flash_size_CONFIG;
	size_t block_size_CONFIG;

	if ((t->cpuid & CPUID_PARTNO_MASK) == CORTEX_M0)
	{
		uChipID = target_mem_read32(t, NUMICRO_CHIP_ID_ADDRESS);
		DEBUG_INFO("Read CHIP ID = 0x%08X\n", uChipID);
	}

	switch (uChipID)
	{
	case 0x01132D00:

		// Nuvoton M032LD2AE
		t->driver = "M032LD2AE";

		ram_size_INTERNAL = 0x2000;

		flash_size_APROM = NUMICRO_APROM_SIZE;
		block_size_APROM = NUMICRO_PAGESIZE;

		// flash_size_DATA = 0x40000;
		// block_size_DATA = 0x800;

		flash_size_LDROM = 0x800;
		block_size_LDROM = NUMICRO_PAGESIZE;

		flash_size_CONFIG = 12;
		block_size_CONFIG = 4;

		// ANUP : TMP CODE
		//bCheck = nu_m032_reg_unlock(t);
		//if (bCheck != true)
		//{
		//	return false;
		//}

		break;

	default:

		// NONE
		t->idcode = stored_idcode;
		return false;
	}

	// M032 specific definitions
	target_add_ram(t, 0x20000000, ram_size_INTERNAL);
	nu_m032_add_flash(t, NUMICRO_APROM_BASE, flash_size_APROM, block_size_APROM);
	// nu_m032_add_flash(t, NUMICRO_DATA_BASE, flash_size_DATA, block_size_DATA);
	nu_m032_add_flash(t, NUMICRO_LDROM_BASE, flash_size_LDROM, block_size_LDROM);
	nu_m032_add_flash(t, NUMICRO_CONFIG_BASE, flash_size_CONFIG, block_size_CONFIG);

	target_add_commands(t, nu_m032_cmd_list, "M032xxxxx");

	return true;
}
