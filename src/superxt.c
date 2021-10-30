/* This is the Chips and Technologies 82c100 chipset used in the Amstrad PC5086 */
#include "ibm.h"
#include "cpu.h"
#include "io.h"
#include "x86.h"
#include "mem.h"

#include "superxt.h"

/* SuperXT registers and EMS data */
typedef struct superxt_t {
	uint8_t regs[256];
	uint8_t regs_index;
	uint8_t ems_reg[4];
	mem_mapping_t mapping[4];
	uint32_t page_exec[4];
	uint16_t ems_port;
	uint32_t ems_base;
	int32_t  ems_pages;
} superxt_t;

static superxt_t superxt;

/* Given an EMS page ID, return its physical address in RAM. */
uint32_t superxt_ems_execaddr(superxt_t *sys, int pg, uint16_t val)
{
	if (!(val & 0x80)) return 0;	/* Bit 7 reset => not mapped */
	if (!sys->ems_pages) return 0;	/* No EMS available */

	val &= 0x7F;

	// pclog("Select EMS page: %d of %d\n", val, sys->ems_pages); 
	if (val < sys->ems_pages)
	{
		/* EMS is any memory above 640k, with val giving the 16k page number */
		return (640 * 1024) + (0x4000 * val);
	}
	return 0;
}


static uint8_t superxt_ems_in(uint16_t addr, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;
	int pg = (addr >> 14) & 3;
	/* EMS registers are not in order in the 64K window:
	 *	register 0 maps at 0x0000
	 *	register 1 maps at 0x8000
	 *	register 2 maps at 0x4000
	 *	register 3 maps at 0xC000
	 *
	 * Swap bit 0 and bit 1 of page number to restore correct order */
	pg = ((pg >> 1 ) | (pg << 1)) & 3;

	// pclog("superxt_ems_in(%04x)=%02x\n", addr, sys->ems_reg[pg]); 
	return sys->ems_reg[pg];
}

static void superxt_ems_out(uint16_t addr, uint8_t val, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;
	int pg = (addr >> 14) & 3;
	/* EMS registers are not in order in the 64K window:
	 *	register 0 maps at 0x0000
	 *	register 1 maps at 0x8000
	 *	register 2 maps at 0x4000
	 *	register 3 maps at 0xC000
	 *
	 * Swap bit 0 and bit 1 of page number to restore correct order */
	pg = ((pg >> 1 ) | (pg << 1)) & 3;

	// pclog("superxt_ems_out(%04x, %02x) pg=%d\n", addr, val, pg); 
	sys->ems_reg[pg] = val;
	sys->page_exec[pg] = superxt_ems_execaddr(sys, pg, val);
	if (sys->page_exec[pg]) /* Page present */
	{
		mem_mapping_enable(&sys->mapping[pg]);
		mem_mapping_set_exec(&sys->mapping[pg], ram + sys->page_exec[pg]);
		flushmmucache();
	}
	else
	{
		mem_mapping_disable(&sys->mapping[pg]);
		flushmmucache();
	}
}

static void superxt_ems_set_config(superxt_t *sys, uint8_t val)
{
	int n;

	// pclog("superxt_ems_set_config(%d)\n", val); 
	if (sys->ems_port)
	{
		for (n = 0; n <= 0xC000; n += 0x4000)
		{
			io_removehandler(sys->ems_port + n, 0x01, 
				superxt_ems_in, NULL, NULL, superxt_ems_out, NULL, NULL, sys);
		}
		sys->ems_port = 0;
	}
	
	sys->ems_port = 0x208 | (val & 0xF0);
	for (n = 0; n <= 0xC000; n += 0x4000)
	{
		io_sethandler(sys->ems_port + n, 0x01, 
			superxt_ems_in, NULL, NULL, superxt_ems_out, NULL, NULL, sys);
	}
	// pclog("EMS port -> %04x\n", sys->ems_port); 
	val &= 0x0F;
	sys->ems_base = 0xC0000 + 0x4000 * val;
	/* Map the EMS page frame */
	for (int pg = 0; pg < 4; pg++)
	{
		mem_mapping_set_addr(&sys->mapping[pg], sys->ems_base + (0x4000 * pg), 16384);
		/* Start them all off disabled */
		mem_mapping_disable(&sys->mapping[pg]);
		sys->ems_reg[pg] = 0;
	}
	flushmmucache();
	// pclog("EMS base -> %05x\n", sys->ems_base); 
}

static int superxt_addr_to_page(uint32_t addr, superxt_t *sys)
{
	int pg = ((addr - sys->ems_base) >> 14);
	
	return pg;
}

/* Read RAM in the EMS page frame */
static uint8_t superxt_ems_read_ram(uint32_t addr, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;
	int pg = superxt_addr_to_page(addr, sys);

	// if (pg < 0 || pg > 3) return 0xFF;
	addr = sys->page_exec[pg] + (addr & 0x3FFF);
	return ram[addr];	
}


static uint16_t superxt_ems_read_ramw(uint32_t addr, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;
	int pg = superxt_addr_to_page(addr, sys);

	// if (pg < 0 || pg > 3) return 0xFF;
	// pclog("superxt_ems_read_ramw addr=%05x ", addr);
	addr = sys->page_exec[pg] + (addr & 0x3FFF);
	// pclog("-> %06x val=%04x\n", addr, *(uint16_t *)&ram[addr]);	 
	return *(uint16_t *)&ram[addr];	
}


static uint32_t superxt_ems_read_raml(uint32_t addr, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;
	int pg = superxt_addr_to_page(addr, sys);

	// if (pg < 0 || pg > 3) return 0xFF;
	addr = sys->page_exec[pg] + (addr & 0x3FFF);
	return *(uint32_t *)&ram[addr];	
}

/* Write RAM in the EMS page frame */
static void superxt_ems_write_ram(uint32_t addr, uint8_t val, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;
	int pg = superxt_addr_to_page(addr, sys);

	if (pg < 0 || pg > 3) return;
	addr = sys->page_exec[pg] + (addr & 0x3FFF);
	ram[addr] = val;	
}


static void superxt_ems_write_ramw(uint32_t addr, uint16_t val, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;
	int pg = superxt_addr_to_page(addr, sys);

	// if (pg < 0 || pg > 3) return;
	// pclog("superxt_ems_write_ramw addr=%05x ", addr); 
	addr = sys->page_exec[pg] + (addr & 0x3FFF);
	// pclog("-> %06x val=%04x\n", addr, val); 

	*(uint16_t *)&ram[addr] = val;	
}


static void superxt_ems_write_raml(uint32_t addr, uint32_t val, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;
	int pg = superxt_addr_to_page(addr, sys);

	// if (pg < 0 || pg > 3) return;
	addr = sys->page_exec[pg] + (addr & 0x3FFF);
	*(uint32_t *)&ram[addr] = val;	
}


void superxt_write(uint16_t port, uint8_t val, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;
	
	switch (port)
        {
                case 0x22:
                sys->regs_index = val;
                break;
                
                case 0x23:
                sys->regs[sys->regs_index] = val;
                switch(sys->regs_index)
                {
			case 0x4C:	/* EMS configuration register */
                	superxt_ems_set_config(sys, val);
                	break;
                	
                	case 0x40:	/* Clock/Mode Size */
                	cpu_set_turbo((val & 0x80) ? 1 : 0);
                	break;
                }                
                break;
        }
}

uint8_t superxt_read(uint16_t port, void *priv)
{
	superxt_t *sys = (superxt_t *)priv;

        switch (port)
        {
                case 0x22:
                return sys->regs_index;
                
                case 0x23:
                return sys->regs[sys->regs_index];
        }
        return 0xff;
}

void superxt_init()
{
	/* Clear configuration */
	memset(&superxt, 0, sizeof(superxt));
		
	/* Compute the number of available EMS pages */
	superxt.ems_pages = ((mem_size - 640) / 16);
	if (superxt.ems_pages < 0) superxt.ems_pages = 0;
	/* Map the EMS page frame at default segment D000 */
	for (int pg = 0; pg < 4; pg++)
	{
		mem_mapping_add(&superxt.mapping[pg], 
			0xD0000 + (0x4000 * pg), 16384, 
			superxt_ems_read_ram,  superxt_ems_read_ramw,  superxt_ems_read_raml,
			superxt_ems_write_ram, superxt_ems_write_ramw, superxt_ems_write_raml,
			NULL, MEM_MAPPING_EXTERNAL, 
			&superxt);
		/* Start them all off disabled */
		mem_mapping_disable(&superxt.mapping[pg]);
	}
	/* Set EMS port address and base address */
	superxt_ems_set_config(&superxt, 0x00);

        io_sethandler(0x0022, 0x0002, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  &superxt); 
}
