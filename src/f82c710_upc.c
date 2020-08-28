/*
 * Chips & Technologies F82C710 Universal Peripheral Controller (UPC)
 *
 * Copyright (c) 2020 Eluan Costa Miranda <eluancm@gmail.com> All rights reserved.
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
 *
 * =============================================================================
 *
 * This SuperIO chip is commonly seem paired with the Single-chip AT (SCAT)
 * chip. Only the functionality needed for the Hyundai Super-286TR BIOS is
 * implented for now.
 *
 * One of the goals of this chip was getting rid of jumpers and have everything
 * configured by software, but there is no configuration for serial and
 * parallel IRQs. Because of that, motherboards still supply jumpers for these
 * two signals.
 *
 */

#include "ibm.h"

#include "fdc.h"
#include "ide.h"
#include "io.h"
#include "lpt.h"
#include "serial.h"
 
#define UPC_MOUSE_DEV_IDLE     0x01      /* Device Idle */
#define UPC_MOUSE_RX_FULL      0x02      /* Device Char received */
#define UPC_MOUSE_TX_IDLE      0x04      /* Device XMIT Idle */
#define UPC_MOUSE_RESET        0x08      /* Device Reset */
#define UPC_MOUSE_INTS_ON      0x10      /* Device Interrupt On */
#define UPC_MOUSE_ERROR_FLAG   0x20      /* Device Error */
#define UPC_MOUSE_CLEAR        0x40      /* Device Clear */
#define UPC_MOUSE_ENABLE       0x80      /* Device Enable */

typedef struct upc_t
{
        int configuration_state; // state of algorithm to enter configuration mode
        int configuration_mode;
        uint8_t next_value;     // next expected value of configuration algorithm
        uint16_t cri_addr; // cri = configuration index register, addr is even
        uint16_t cap_addr; // cap = configuration access port, addr is odd and is cri_addr + 1
        uint8_t cri; // currently indexed register

        // these regs are not affected by reset
        uint8_t regs[15]; // there are 16 indexes, but there is no need to store the last one which is: R = cri_addr / 4, W = exit config mode

        int serial_irq;
        int parallel_irq; // TODO: currently not implemented in PCem

        int mouse_irq;
        uint16_t mdata_addr;    // Address of PS/2 data register 
        uint16_t mstat_addr;    // Address of PS/2 status register 
        uint8_t mouse_status;   // Mouse interface status register
        void (*mouse_write)(uint8_t val, void *p);
        void *mouse_p;
} upc_t;

static upc_t upc;
uint8_t upc_config_read(uint16_t port, void *priv);
void upc_config_write(uint16_t port, uint8_t val, void *priv);

void upc_mouse_disable(upc_t *upc); 
void upc_mouse_enable(upc_t *upc); 
uint8_t upc_mouse_read(uint16_t port, void *priv);
void upc_mouse_write(uint16_t port, uint8_t val, void *priv);

void upc_update_config(upc_t *upc)
{
        switch(upc->cri)
        {
                case 0:
                        if (upc->regs[0] & 0x4)
                        {
                                serial1_set(upc->regs[4] * 4, upc->serial_irq, 0);
                                pclog("UPC: UART at %04X, irq %d\n", upc->regs[4] * 4, upc->serial_irq);
                        }
                        else
                        {
                                serial1_remove();
                                pclog("UPC: UART disabled\n");
                        }
                        if (upc->regs[0] & 0x8)
                        {
                                lpt1_init(upc->regs[6] * 4);
                                pclog("UPC: PARALLEL at %04X, irq %d\n", upc->regs[6] * 4, upc->parallel_irq);
                        }
                        else
                        {
                                lpt1_remove();
                                pclog("UPC: PARALLEL disabled\n");
                        }
                        if ((upc->regs[0] & 0x60) != 0)
                                pclog("UPC: Oscillator control not implemented!\n");
                        break;
                        
                case 1:
                        if ((upc->regs[1] & 0x80) != 0)
                                pclog("UPC: Restricted serial reset not implemented!\n");
                        if ((upc->regs[1] & 0x80) != 0)
                                pclog("UPC: Restricted serial reset not implemented!\n");
                        if ((upc->regs[1] & 0x40) != 0)
                                pclog("UPC: Bidirectional parallel port support not implemented!\n");
                        if ((upc->regs[1] & 0x38) != 0)
                                pclog("UPC: UART force CTS, DSR, DCD not implemented!\n");
                        break;
                
                case 2:            
                        if ((upc->regs[2] & 0x70) != 0)
                                pclog("UPC: UART clock control not implemented!\n");
                        break;
                        
                case 9:
                        if (upc->regs[9] == 0xb0)
                                pclog("UPC: GPCS not implemented! (at default address: %04X)\n", upc->regs[9] * 4);
                        else if (upc->regs[9] != 0)
                                pclog("UPC: GPCS not implemented! (at address: %04X)\n", upc->regs[9] * 4);
                        break;
                        
                case 12:        
                        if ((upc->regs[12] & 0x40) != 0) 
                        {
                                ide_pri_disable();
                                pclog("UPC: IDE XT mode not implemented!\n");        
                        }
                        else 
                        {
                                if (upc->regs[12] & 0x80)
                                {
                                        ide_pri_enable();
                                        pclog("UPC: AT IDE enabled\n");
                                }
                                else
                                {       
                                        ide_pri_disable();
                                        pclog("UPC: AT IDE disabled\n");
                                }        
                        }
        
                        if (upc->regs[12] & 0x20)
                        {
                                /* Adding the floppy controller when it is already present causes problems.*/
                                /* FIX: remove it before adding it */
                                fdc_remove();   
                                fdc_add();
                                pclog("UPC: FDC enabled\n");
                        }
                        else
                        {
                                fdc_remove();
                                pclog("UPC: FDC disabled\n");
                        }

                        if ((upc->regs[12] & 0x10) != 0)
                                pclog("UPC: FDC power down mode not implemented!\n");
                        if ((upc->regs[12] & 0x0C) != 0)
                                pclog("UPC: RTCCS not implemented!\n");
                        if ((upc->regs[12] & 0x01) != 0)
                                pclog("UPC: PS/2 mouse port power down not implemented!\n");
                        break;
                        
                case 13:
                if (upc->regs[13] != 0) {
                        upc->mdata_addr = upc->regs[13] * 4;
                        upc->mstat_addr = upc->mdata_addr + 1;
                        pclog("UPC: PS/2 mouse port at %04X, irq %d\n", upc->mdata_addr, upc->mouse_irq);
                        upc_mouse_enable(upc);                
                } else {              
                        pclog("UPC: PS/2 mouse port disabled\n");
                }
                
                case 14:
                        if (upc->regs[14] != 0)
                                pclog("UPC: Test mode not implemented!\n");
                        break;
        }
}

uint8_t upc_config_read(uint16_t port, void *priv)
{
        upc_t *upc = (upc_t *)priv;
        uint8_t temp = 0xff;

        if (upc->configuration_mode)
        {
                if (port == upc->cri_addr)
                {
                        temp = upc->cri;
                }
                else if (port == upc->cap_addr)
                {
                        if (upc->cri == 0xf)
                                temp = upc->cri_addr / 4;
                        else
                                temp = upc->regs[upc->cri];
                }
        }

        // pclog("UPC READ : %04X, %02X\n", port, temp);
        return temp;
}

void upc_config_write(uint16_t port, uint8_t val, void *priv)
{
        upc_t *upc = (upc_t *)priv;
        int configuration_state_event = 0;

        // pclog("UPC WRITE: %04X, %02X\n", port, val);

        switch(port)
        {
                case 0x2fa:
                        /* Execute configuration step 1 for any value except 9, ff or 36 */
                        if (upc->configuration_state == 0)
                        {
                                configuration_state_event = 1;
                                /* next value should be the 1's complement of the current one */
                                upc->next_value = 0xff - val; 
                        }
                        else if (upc->configuration_state == 4)
                        {
                                uint8_t addr_verify = upc->cri_addr / 4;
                                addr_verify += val;
                                if (addr_verify == 0xff)
                                {
                                        upc->configuration_mode = 1;
                                        // TODO: is the value of cri reset here or when exiting configuration mode?
                                        io_sethandler(upc->cri_addr, 0x0002, upc_config_read, NULL, NULL, upc_config_write, NULL, NULL, upc);
                                        pclog("UPC: in configuration mode at %04X\n", upc->cri_addr);
                                }
                                else
                                {
                                        upc->configuration_mode = 0;
                                        pclog("UPC: configuration mode failed (sum = %02X)\n", addr_verify);
                                }
                        }
                        break;
                case 0x3fa:
                        /* go to configuration step 2 if value is the expected one */
                        if (upc->configuration_state == 1 && val == upc->next_value)
                                configuration_state_event = 1;
                        else if (upc->configuration_state == 2 && val == 0x36)
                                configuration_state_event = 1;
                        else if (upc->configuration_state == 3)
                        {
                                upc->cri_addr = val * 4;
                                upc->cap_addr = upc->cri_addr + 1;
                                configuration_state_event = 1;
                        }
                        break;
                default:
                        break;
        }
        if (upc->configuration_mode)
        {
                if (port == upc->cri_addr)
                {
                        upc->cri = val & 0xf;

                }
                else if (port == upc->cap_addr)
                {
                        if (upc->cri == 0xf)
                        {
                                pclog("UPC: exiting configuration mode\n");
                                upc->configuration_mode = 0;
                                io_removehandler(upc->cri_addr, 0x0002, upc_config_read, NULL, NULL, upc_config_write, NULL, NULL, upc);
                        }
                        else
                        {
                                upc->regs[upc->cri] = val;
                                /* configuration should be updated at each register write, otherwise PC5086 do not detect ports correctly */
                                upc_update_config(upc);                        
                        }
                }
        }

        // TODO: is the state only reset when accessing 0x2fa and 0x3fa wrongly?
        if ((port == 0x2fa || port == 0x3fa) && configuration_state_event)
                upc->configuration_state++;
        else
                upc->configuration_state = 0;
}

static void *upc_init()
{
        pclog("UPC INIT\n");

        /* Disable all peripherals. upc_update_config will enable configured peripherals */
        serial1_remove();
        serial2_remove();
        lpt1_remove();
        lpt2_remove();
        fdc_remove();
        ide_pri_disable();
        ide_sec_disable();

        memset(&upc, 0, sizeof(upc));

        upc.serial_irq = device_get_config_int("serial_irq");
        upc.parallel_irq = device_get_config_int("parallel_irq");
        upc.mouse_irq = 0x2;    /* set default for Amstrad PC5086 */

        // because of these addresses, serial ports must be 16450 without fifos
        io_sethandler(0x02fa, 0x0001, NULL, NULL, NULL, upc_config_write, NULL, NULL, &upc);
        io_sethandler(0x03fa, 0x0001, NULL, NULL, NULL, upc_config_write, NULL, NULL, &upc);

        upc.regs[0] = 0x0c;
        upc.regs[1] = 0x00;
        upc.regs[2] = 0x00;
        upc.regs[3] = 0x00;
        upc.regs[4] = 0xfe;
        upc.regs[5] = 0x00;
        upc.regs[6] = 0x9e;
        upc.regs[7] = 0x00;
        upc.regs[8] = 0x00;
        upc.regs[9] = 0xb0;
        upc.regs[10] = 0x00;
        upc.regs[11] = 0x00;
        upc.regs[12] = 0xa0;
        upc.regs[13] = 0x00;
        upc.regs[14] = 0x00;

        for(upc.cri = 0; upc.cri < 15; upc.cri++)
            upc_update_config(&upc);
        upc.cri = 0;
        return &upc;
}

static device_config_t upc_config[] =
{
        {
                .name = "serial_irq",
                .description = "Serial Port IRQ",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "IRQ 4 (for address 0x3F8/COM1)",
                                .value = 4
                        },
                        {
                                .description = "IRQ 3 (for address 0x2F8/COM2)",
                                .value = 3
                        },
                        {
                                .description = "Disabled",
                                .value = 0 // TODO: see if this breaks PCem's PIC
                        }
                },
                .default_int = 4
        },
        {
                .name = "parallel_irq",
                .description = "Parallel Port IRQ",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "IRQ 7 (for address 0x378/LPTB)",
                                .value = 7
                        },
                        {
                                .description = "IRQ 5 (for address 0x278/LPTC)",
                                .value = 5
                        },
                        {
                                .description = "Disabled",
                                .value = 0 // TODO: see if this breaks PCem's PIC
                        }
                },
                .default_int = 7
        },
        {
                .name = "parallel_irq",
                .description = "Parallel Port IRQ",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "IRQ 7 (for address 0x378/LPTB)",
                                .value = 7
                        },
                        {
                                .description = "IRQ 5 (for address 0x278/LPTC)",
                                .value = 5
                        },
                        {
                                .description = "Disabled",
                                .value = 0 // TODO: see if this breaks PCem's PIC
                        }
                },
                .default_int = 7
        },
        // For the Hyundai Super-286TR, the only other jumper is the Color/Mono one and it's handled by the at keyboard controller code.
        {
                .type = -1
        }
};


device_t f82c710_upc_device =
{
        "F82C710 UPC",
        0,
        upc_init,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        upc_config
};

/****************** PS/2 mouse port *********************/
uint8_t upc_mouse_read(uint16_t port, void *priv)
{
        upc_t *upc = (upc_t *)priv;
        uint8_t temp = 0xff;
        if(port == upc->mstat_addr) {
            temp = upc->mouse_status;
        }
        
        pclog("UPC mouse READ : %04X, %02X\n", port, temp);
        return temp;
}

void upc_mouse_write(uint16_t port, uint8_t val, void *priv)
{
        upc_t *upc = (upc_t *)priv;
        if(port == upc->mstat_addr) {
            upc->mouse_status = val;
        }

        pclog("UPC mouse WRITE: %04X, %02X\n", port, val);
}

void upc_mouse_disable(upc_t *upc) 
{
    io_removehandler(upc->mdata_addr, 0x0002, upc_mouse_read, NULL, NULL, upc_mouse_write, NULL, NULL, upc);                
}

void upc_mouse_enable(upc_t *upc)
{
    io_sethandler(upc->mdata_addr, 0x0002, upc_mouse_read, NULL, NULL, upc_mouse_write, NULL, NULL, upc);
}

void upc_set_mouse(void (*mouse_write)(uint8_t val, void *p), void *p)
{
        upc.mouse_write = mouse_write;
        upc.mouse_p = p;
}
