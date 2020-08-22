/* This is the Chips and Technologies C82100 chipset used in the Amstrad PC5086 model */
#include "ibm.h"
#include "io.h"
#include "x86.h"
#include "serial.h"
#include "lpt.h"
#include "superxt.h"

static uint8_t superxt_regs[256];
static int superxt_index;
static int superxt_emspage[4];
/* Set initial values of 82C610 registers */
static uint8_t c601_regs[16] = {0x00, 0x00, 0x08, 0x00, 0xFE, 0xBE, 0x9E, 0x00,
                                0xEC, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB7};
static int c601_index;

void superxt_set_ports()
{
        /* check if COM1 is enabled */
        if(c601_regs[0] & 4)
        {
                /* get I/O address from register 4 */
                int addr = (c601_regs[4] << 2);
                /* hardcode to IRQ 4 */
                int irq = 4;
                serial1_set(addr, irq);
                pclog("Enable COM1 with address %03X and IRQ %d\n", addr, irq);
        } else {
                pclog("Disable COM1\n");
                serial1_remove();
        }
        /* check if COM2 is enabled */
        if(c601_regs[0] & 2)
        {
                /* get I/O address from register 5 */
                int addr = (c601_regs[5] << 2);
                /* hardcode to IRQ 3 */
                int irq = 3;
                serial2_set(addr, irq);
                pclog("Enable COM2 with address %03X and IRQ %d\n", addr, irq);
        } else {
                pclog("Disable COM2\n");
                serial2_remove();
        }
        /* check if LPT is enabled */
        if(c601_regs[0] & 8)
        {
                /* get I/O address from register 6 */
                int addr = (c601_regs[6] << 2);
                lpt1_init(addr);
                pclog("Enable COM2 with address %03X\n", addr);
        } else {
                pclog("Disable LPT\n");
                lpt1_remove();
        }}

void superxt_write(uint16_t port, uint8_t val, void *priv)
{
        switch (port)
        {
                case 0x22:
                pclog("Write config reg %03X %02X %04X:%04X\n",port,val,CS,PC);
                superxt_index = val;
                break;
                
                case 0x23:
                pclog("Write config reg %03X %02X %04X:%04X\n",port,val,CS,PC);
                superxt_regs[superxt_index] = val;
                break;

                case 0x0208: case 0x4208: case 0x8208: case 0xC208: 
                pclog("Write EMS %03X %02X %04X:%04X\n",port,val,CS,PC);
                superxt_emspage[port >> 14] = val;                
                break;

                case 0x2DC: 
                pclog("82C601 write %03X %02X %04X:%04X\n", port, val, CS, PC);
                c601_index = val & 0x0F; /* select only low bits of register index */
                break;
                                
                case 0x2DD:
                pclog("82C601 write %03X %02X %04X:%04X\n", port, val, CS, PC);
                if(c601_index == 0x0F)
                {
                        superxt_set_ports();       
                } else {
                        c601_regs[c601_index] = val;                
                }
                break;    
                            
                /* debug COM port detection */
                default:
                pclog("SuperXT write serial %03X %02X %04X:%04X\n",port,val,CS,PC);                
        }
}

uint8_t superxt_read(uint16_t port, void *priv)
{
        switch (port)
        {
                case 0x22:
                pclog("Read config reg %03X %04X:%04X\n", port, CS, PC);
                return superxt_index;
                
                case 0x23:
                pclog("Read config reg %03X %04X:%04X\n", port, CS, PC);
                return superxt_regs[superxt_index];
                
                case 0x0208: case 0x4208: case 0x8208: case 0xC208: 
                pclog("Read EMS %03X %04X:%04X\n", port, CS, PC);
                return superxt_emspage[port >> 14];                

                case 0x2DC: 
                pclog("82C601 Read %03X %04X:%04X\n", port, CS, PC);
                return c601_index;
                break;
                
                case 0x2DD:
                pclog("82C601 Read %03X %04X:%04X\n", port, CS, PC);
                return c601_regs[c601_index];
                break;
                
                default:
                pclog("SuperXT Read serial %03X %04X:%04X\n", port, CS, PC);
                return 0xff;
        }
        return 0xff;
}

void superxt_init()
{
        io_sethandler(0x0022, 0x0002, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);
        io_sethandler(0x0208, 0x0001, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);
        io_sethandler(0x4208, 0x0001, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);
        io_sethandler(0x8208, 0x0001, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);
        io_sethandler(0xc208, 0x0001, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);
        io_sethandler(0x2DC, 0x0002, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);        
        /* debug com port */
        // io_sethandler(0x3F8, 0x0008, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);        
        // io_sethandler(0x2F8, 0x0008, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);        
        // io_sethandler(0x338, 0x0008, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);        
        // io_sethandler(0x328, 0x0008, superxt_read, NULL, NULL, superxt_write, NULL, NULL,  NULL);        
}
