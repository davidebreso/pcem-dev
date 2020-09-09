/*Chips and Technologies 82C451 emulation*/
#include <stdlib.h>
#include "ibm.h"
#include "device.h"
#include "io.h"
#include "mem.h"
#include "rom.h"
#include "video.h"
#include "vid_ct451.h"
#include "vid_svga.h"

/* Enable debug log */
int ct451log = 0;

typedef struct ct451_t
{
        svga_t svga;
        rom_t bios_rom;
        
        /* Setup registers */
        uint8_t sleep;      /* Video subsystem sleep */
        uint8_t xena;       /* Extended Enable */
        uint8_t xrx;        /* Extension index register */
        uint8_t xreg[128];  /* Extension registers */
        uint8_t setup;      /* Setup control register */
        
} ct451_t;

int ct451_wp_group0(ct451_t *ct451)
{
        /* Group 0 write protection is enabled if CR11 bit 7 OR XR15 bit 6 are 1 */
        svga_t *svga = &ct451->svga;
        return (svga->crtc[0x11] & 0x80) || (ct451->xreg[0x15] & 0x40);
}


void ct451_out(uint16_t addr, uint8_t val, void *p)
{
        ct451_t *ct451 = (ct451_t *)p;
        svga_t *svga = &ct451->svga;
        uint8_t old;

        /* if VGA chip is disabled, respond only to ports 0x10x and setup control */
        if (!(ct451->setup & 8) && ((addr&0xFF00) == 0x300))
                return;

        /* Extension registers address selection. 
         * Address depends on bit 6 of Extension enable register:
         *      0: address 0x3D6/0x3D7
         *      1: address 0x3B6/0x3B7
         */
        if ((addr&0xFFFE) == 0x3D6 || (addr&0xFFFE) == 0x3B6) 
        {
                if(ct451->xena & 0x40)
                {
                        if(ct451log) pclog("%04X->", addr);
                        addr ^= 0x60;        
                        if(ct451log) pclog("%04X ", addr);
                }
        }
        /* mono / color addr selection */
        else if (((addr&0xFFF0) == 0x3D0 || (addr&0xFFF0) == 0x3B0) && !(svga->miscout & 1))
        {
                if(ct451log) pclog("%04X->", addr);
                addr ^= 0x60;        
                if(ct451log) pclog("%04X ", addr);        
        }

        if(ct451log) pclog("ct451_out : %04X %02X  %02X %i ", addr, val, ram[0x489], ins);
        if(ct451log) pclog("  %04X:%04X\n", CS,cpu_state.pc); 
        
        switch(addr)
        {
                case 0x102:     /* Video subsystem sleep control */
                ct451->sleep = val;
                break;
                
                case 0x103:     /* Extension Enable Register */
                /* The register is available only in Setup mode (bit 4 of setup register set) */
                if(ct451->setup & 0x10)
                        ct451->xena = val;
                break;
            
                case 0x104:     /* Global ID (read-only) */
                break;

                case 0x3D4:     /* CRTC Index register */
                svga->crtcreg = val & 0x3f;
                return;
                
                case 0x3D5:     /* CRCT Register data */
                if (svga->crtcreg > 0x18)
                {
                        if(ct451log) pclog("Write to undocumented CRTC register %02X\n", svga->crtcreg);
                        return;                
                }
                /* If group protect 0 is enabled, disable write to CR00-CR06 */
                if ((svga->crtcreg < 7) && ct451_wp_group0(ct451)) 
                        return;
                /* If group protect 0 is enabled, enable write only to bit 4 of CR07 */
                if ((svga->crtcreg == 7) && ct451_wp_group0(ct451))
                        val = (svga->crtc[7] & ~0x10) | (val & 0x10);
                old = svga->crtc[svga->crtcreg];
                svga->crtc[svga->crtcreg] = val;
                if (old != val)
                {
                        if (svga->crtcreg < 0xe || svga->crtcreg > 0x10)
                        {
                                svga->fullchange = changeframecount;
                                svga_recalctimings(svga);
                        }
                }
                break;

                case 0x3D6:     /* Extension index register. Active only when bit 7 of XENA = 1 */
                if(ct451->xena & 0x80)
                    ct451->xrx = val & 0x7F;
                break;
            
                case 0x3D7:     /* Extension register data. Active only when bit 7 of XENA = 1 */
                if(ct451->xena & 0x80)
                    ct451->xreg[ct451->xrx] = val; 
                break;               
                
                case 0x46E8:    /* Setup control register (write only) */
                ct451->setup = val;
                break;                
                
                default:
                svga_out(addr, val, svga);            
        }       

}

uint8_t ct451_in(uint16_t addr, void *p)
{
        ct451_t *ct451 = (ct451_t *)p;
        svga_t *svga = &ct451->svga;
        uint8_t temp = 0xff;

        /* if VGA chip is disabled, respond only to ports 0x10x and setup control */
        if (!(ct451->setup & 8) && ((addr&0xFF00) == 0x300))
                return temp;

        /* Extension registers address selection. 
         * Address depends on bit 6 of Extension enable register:
         *      0: address 0x3D6/0x3D7
         *      1: address 0x3B6/0x3B7
         */
        if ((addr&0xFFFE) == 0x3D6 || (addr&0xFFFE) == 0x3B6) 
        {
                if(ct451->xena & 0x40)
                {
                        if(ct451log) pclog("%04X->", addr);
                        addr ^= 0x60;        
                        if(ct451log) pclog("%04X ", addr);
                }
        }
        /* mono / color addr selection */
        else if (((addr&0xFFF0) == 0x3D0 || (addr&0xFFF0) == 0x3B0) && !(svga->miscout & 1))
        {
                if(ct451log) pclog("%04X->", addr);
                addr ^= 0x60;        
                if(ct451log) pclog("%04X ", addr);        
        }
        
        switch(addr)
        {
                case 0x102:     /* Video subsystem sleep control */
                temp = ct451->sleep;
                break;
            
                case 0x103:     /* Extension Enable Register */
                /* The register is available only in Setup mode (bit 4 of setup register set) */
                if(ct451->setup & 0x10)
                        temp = ct451->xena;
                break;
            
                case 0x104:     /* Global ID (0xA5 read-only) */
                temp = 0xA5;
                break;

                case 0x3D4:
                temp = svga->crtcreg;
                break;
            
                case 0x3D5:
                if (svga->crtcreg > 0x18)
                {
                        if(ct451log) pclog("Read from undocumented CRTC register %02X\n", svga->crtcreg);
                        temp = 0xff;                
                } 
                else
                    temp = svga->crtc[svga->crtcreg & 31];
                break;
          
                case 0x3D6:     /* Extension index register. Active only when bit 7 of XENA = 1 */
                if(ct451->xena & 0x80)
                    temp = ct451->xrx;
                break;
            
                case 0x3D7:     /* Extension register data. Active only when bit 7 of XENA = 1 */
                if(ct451->xena & 0x80)
                    temp = ct451->xreg[ct451->xrx];                
                break;

                case 0x46E8:    /* Setup control register (write only) */
                temp = 0xff;
                break;                
            
                default:
                temp = svga_in(addr, svga);
        }
        
        if(ct451log) pclog("ct451_in : %04X %02X  %02X %i ", addr, temp, ram[0x489], ins);
        if(ct451log) pclog("  %04X:%04X\n", CS,cpu_state.pc);        

        return temp;
}

void *ct451_common_init(char *bios_fn, int vram_size)
{
        ct451_t *ct451 = malloc(sizeof(ct451_t));
        memset(ct451, 0, sizeof(ct451_t));
        if(ct451log) pclog("CT451: setting up BIOS from %s\n", bios_fn);
        rom_init(&ct451->bios_rom, bios_fn, 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);

        if(ct451log) pclog("CT451: calling SVGA init\n");
        svga_init(&ct451->svga, ct451, vram_size << 10,
                   NULL,
                   ct451_in, ct451_out,
                   NULL,
                   NULL);

        if(ct451log) pclog("CT451: setting up I/O handler\n");
        /* handler for setup registers */
        io_sethandler(0x0100, 0x0005, ct451_in, NULL, NULL, ct451_out, NULL, NULL, ct451);
        /* handler for VGA registers */
        io_sethandler(0x03c0, 0x0020, ct451_in, NULL, NULL, ct451_out, NULL, NULL, ct451);
        /* handler for setup control register */
        io_sethandler(0x46E8, 0x0001, ct451_in, NULL, NULL, ct451_out, NULL, NULL, ct451);
            
        /* Set default register values */
        // ct451->svga.miscout = 1;
        ct451->xreg[0x00] = 0x4;    /* Chip version */
        ct451->xreg[0x01] = 0x5A;   /* DIP switch */
        ct451->xreg[0x28] = 0x2;    /* Video interface */
        
        return ct451;
}

void *ct451_init()
{
        return ct451_common_init("ct451/c000.bin", 256);
}

static int ct451_available()
{
        return rom_present("ct451/c000.bin");
}

void ct451_close(void *p)
{
        if(ct451log) pclog("ct451_close %08X\n", p);

        ct451_t *ct451 = (ct451_t *)p;

        svga_close(&ct451->svga);

        free(ct451);
}

void ct451_speed_changed(void *p)
{
        if(ct451log) pclog("ct451_speed_changed %08X\n", p);

        ct451_t *ct451 = (ct451_t *)p;

        svga_recalctimings(&ct451->svga);
}

void ct451_force_redraw(void *p)
{
        if(ct451log) pclog("ct451_force_redraw %08X\n", p);

        ct451_t *ct451 = (ct451_t *)p;

        ct451->svga.fullchange = changeframecount;
}

void ct451_add_status_info(char *s, int max_len, void *p)
{
        if(ct451log) pclog("ct451_add_status_info %08X\n", p);

        ct451_t *ct451 = (ct451_t *)p;

        svga_add_status_info(s, max_len, &ct451->svga);
}

device_t ct451_device =
{
        "Chips and Technologies 82C451",
        0,
        ct451_init,
        ct451_close,
        ct451_available,
        ct451_speed_changed,
        ct451_force_redraw,
        ct451_add_status_info
};
