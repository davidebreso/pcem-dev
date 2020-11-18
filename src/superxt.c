/*This is the Chips and Technologies C82100 chipset used in the Amstrad PC5086 model*/
#include "ibm.h"
#include "io.h"
#include "x86.h"
#include "mem.h"
#include "superxt.h"

static uint8_t superxt_regs[256];
static int superxt_index;
static int superxt_emspage[4] = {0x0, 0x0, 0x0, 0x0};
static int superxt_emsbase = 0xD0000;
static mem_mapping_t superxt_ems_mapping[4];

uint32_t get_superxt_ems_addr(uint32_t addr)
{
        // pclog("get_superxt_ems_addr(%08X) called.\n", addr);
        if(addr < superxt_emsbase || addr >= superxt_emsbase + 0x10000)
        {
                pclog("WARNING: read/write outside EMS window at address %08X\n", addr);
                return addr;
        }
        addr = addr - superxt_emsbase;      /* Address relative to EMS window */
        int page_index = (addr >> 14) & 3;   /* Page number */
        uint32_t page_base = (superxt_emspage[page_index] & 0x7F) << 14;    /* bit 7 is enabled/disabled */
        addr = addr & 0x3FFF;               /* Address relative to 16K page */
        // pclog("page number %d, base %08X, offset %04X\n", page_index, page_base, addr);
        addr = page_base + addr + 0xA0000;  /* Phyisical address */
        // pclog("physical address %08X\n", addr);        
        return addr;
}

static void mem_writeb_superxtems(uint32_t addr, uint8_t val, void *priv)
{
        // pclog("mem_writeb_superxtems(%08X, %04X, %p) called. ", addr, val, priv);
        int page_index = (addr >> 14) & 3;
        if(!(superxt_emspage[page_index] & 0x80))
        {
                pclog("WARNING: write to a disabled EMS page at address %08X\n", addr);
                return;                
        }
        addr = get_superxt_ems_addr(addr);
        // pclog("Physical address is %08X\n", addr);
        if (addr < (mem_size << 10))
                ram[addr] = val;
}

static uint8_t mem_readb_superxtems(uint32_t addr, void *priv)
{
        uint8_t val = 0xFF;
        
        // pclog("mem_readb_superxtems(%08X, %p) called. ", addr, priv);
        int page_index = (addr >> 14) & 3;
        if(!(superxt_emspage[page_index] & 0x80))
        {
                pclog("WARNING: reading a disabled EMS page at address %08X\n", addr);
                return 0xFF;                
        }
        addr = get_superxt_ems_addr(addr);
        // pclog("Physical address is %08X, ", addr);
        if (addr < (mem_size << 10))
                val = ram[addr];
        // pclog("value read %04X\n", val);
        return val;
}



void superxt_write(uint16_t port, uint8_t val, void *priv)
{
        int idx;
        pclog("%04X:%04X SUPERXT WRITE : %04X, %02X\n", CS, cpu_state.pc, port, val);
        switch (port)
        {
                case 0x22:
                superxt_index = val;
                break;
                
                case 0x23:
                superxt_regs[superxt_index] = val;
                break;

                case 0x0208: case 0x4208: case 0x8208: case 0xC208:
                /* swap bits of index to get the correct number (Registers are not in order) */
                idx = (port >> 14) & 3;
                // pclog("Changing EMS page index from %d ", idx);
                idx = ((idx >> 1) & 1) | ((idx << 1) & 2);
                // pclog("to %d\n", idx);
                 
                superxt_emspage[idx] = val;             
                mem_mapping_disable(&superxt_ems_mapping[idx]);
                if(val & 0x80)
                {
                        mem_mapping_enable(&superxt_ems_mapping[idx]);   
                } 
                flushmmucache();                    
                break;                
                
                // default:
                // pclog("%04X:%04X SUPERXT WRITE : %04X, %02X\n", CS, cpu_state.pc, port, val);
        }
}

uint8_t superxt_read(uint16_t port, void *priv)
{
        pclog("%04X:%04X SUPERXT READ : %04X, ", CS, cpu_state.pc, port);
        uint8_t temp = 0xff;
        switch (port)
        {
                case 0x22:
                temp = superxt_index;
                break;
                
                case 0x23:
                temp = superxt_regs[superxt_index];
                break;
                
                case 0x0208: case 0x4208: case 0x8208: case 0xC208: 
                temp = superxt_emspage[port >> 14]; 
                break;                 
        }
        pclog("%02X\n", temp);
        return temp;
}

void superxt_init()
{
        /* Set register 0x42 to invalid configuration at startup */
        superxt_regs[0x42] = 0;

        io_sethandler(0x0022, 0x0002, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);
        io_sethandler(0x0208, 0x0001, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);
        io_sethandler(0x4208, 0x0001, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);
        io_sethandler(0x8208, 0x0001, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);
        io_sethandler(0xc208, 0x0001, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);

        /* Set initial EMS page mapping */
        for (int i = 0; i < 4; i++)
        {
                mem_mapping_add(&superxt_ems_mapping[i], superxt_emsbase + (i << 14), 0x4000, 
                                mem_readb_superxtems, NULL, NULL, 
                                mem_writeb_superxtems, NULL, NULL, 
                                ram + 0xA0000, 0, NULL);
                mem_mapping_disable(&superxt_ems_mapping[i]);
        }
        mem_set_mem_state(superxt_emsbase, 0x10000, MEM_READ_EXTERNAL | MEM_WRITE_EXTERNAL);
}
