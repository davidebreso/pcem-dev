/*
 *              Implementation of a generic IDE-XTA disk controller.
 *
 *              This file is a port from the VARCem Project.
 *
 *              XTA is the acronym for 'XT-Attached', which was basically
 *              the XT-counterpart to what we know now as IDE (which is
 *              also named ATA - AT Attachment.)  The basic ideas was to
 *              put the actual drive controller electronics onto the drive
 *              itself, and have the host machine just talk to that using
 *              a simpe, standardized I/O path- hence the name IDE, for
 *              Integrated Drive Electronics.
 *
 *              In the ATA version of IDE, the programming interface of
 *              the IBM PC/AT (which used the Western Digitial 1002/1003
 *              controllers) was kept, and, so, ATA-IDE assumes a 16bit
 *              data path: it reads and writes 16bit words of data. The
 *              disk drives for this bus commonly have an 'A' suffix to
 *              identify them as 'ATBUS'.
 *
 *              In XTA-IDE, which is slightly older, the programming 
 *              interface of the IBM PC/XT (which used the MFM controller
 *              from Xebec) was kept, and, so, it uses an 8bit data path.
 *              Disk drives for this bus commonly have the 'X' suffix to
 *              mark them as being for this XTBUS variant.
 *
 *              So, XTA and ATA try to do the same thing, but they use
 *              different ways to achive their goal.
 *
 *              Also, XTA is **not** the same as XTIDE.  XTIDE is a modern
 *              variant of ATA-IDE, but retro-fitted for use on 8bit XT
 *              systems: an extra register is used to deal with the extra
 *              data byte per transfer.  XTIDE uses regular IDE drives,
 *              and uses the regular ATA/IDE programming interface, just
 *              with the extra register.
 * 
 * NOTE:        This driver implements both the 'standard' XTA interface,
 *              sold by Western Digital as the WDXT-140 (no BIOS) and the
 *              WDXT-150 (with BIOS), as well as some variants customized
 *              for specific machines.
 *
 * NOTE:        The XTA interface is 0-based for sector numbers !!
 *
 * Version:     @(#)hdc_ide_xta.c       1.0.18  2021/03/16
 *
 * Author:      Fred N. van Kempen, <decwiz@yahoo.com>
 *
 *              Based on my earlier HD20 driver for the EuroPC.
 *
 *              Copyright 2017-2021 Fred N. van Kempen.
 *              Copyright 2020 Altheos.
 *
 *              Redistribution and  use  in source  and binary forms, with
 *              or  without modification, are permitted  provided that the
 *              following conditions are met:
 *
 *              1. Redistributions of  source  code must retain the entire
 *                 above notice, this list of conditions and the following
 *                 disclaimer.
 *
 *              2. Redistributions in binary form must reproduce the above
 *                 copyright  notice,  this list  of  conditions  and  the
 *                 following disclaimer in  the documentation and/or other
 *                 materials provided with the distribution.
 *
 *              3. Neither the  name of the copyright holder nor the names
 *                 of  its  contributors may be used to endorse or promote
 *                 products  derived from  this  software without specific
 *                 prior written permission.
 *
 * THIS SOFTWARE  IS  PROVIDED BY THE  COPYRIGHT  HOLDERS AND CONTRIBUTORS
 * "AS IS" AND  ANY EXPRESS  OR  IMPLIED  WARRANTIES,  INCLUDING, BUT  NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE  ARE  DISCLAIMED. IN  NO  EVENT  SHALL THE COPYRIGHT
 * HOLDER OR  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL,  EXEMPLARY,  OR  CONSEQUENTIAL  DAMAGES  (INCLUDING,  BUT  NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE  GOODS OR SERVICES;  LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED  AND ON  ANY
 * THEORY OF  LIABILITY, WHETHER IN  CONTRACT, STRICT  LIABILITY, OR  TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING  IN ANY  WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#define __USE_LARGEFILE64
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#define off64_t off_t
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <sys/types.h>
#include "ibm.h"

#include "device.h"
#include "dma.h"
#include "hdd_file.h"
#include "io.h"
#include "mem.h"
#include "pic.h"
#include "rom.h"
#include "timer.h"
#include "x86.h"

#include "ide_xta.h"


#define HDC_TIME        (50*TIMER_USEC)
#define XTA_NUM         2                       // #supported drives

#define WD_BIOS_FILE            "idexywd2.bin"
#define PC5086_BIOS_FILE        "pc5086/c800.bin"


extern char ide_fn[7][512];

/* Command values. */
#define CMD_TEST_READY          0x00
#define CMD_RECALIBRATE         0x01
                /* unused       0x02 */
#define CMD_READ_SENSE          0x03
#define CMD_FORMAT_DRIVE        0x04
#define CMD_READ_VERIFY         0x05
#define CMD_FORMAT_TRACK        0x06
#define CMD_FORMAT_BAD_TRACK    0x07
#define CMD_READ_SECTORS        0x08
                /* unused       0x09 */
#define CMD_WRITE_SECTORS       0x0a
#define CMD_SEEK                0x0b
#define CMD_SET_DRIVE_PARAMS    0x0c
#define CMD_READ_ECC_BURST      0x0d
#define CMD_READ_SECTOR_BUFFER  0x0e
#define CMD_WRITE_SECTOR_BUFFER 0x0f
#define CMD_RAM_DIAGS           0xe0
                /* unused       0xe1 */
                /* unused       0xe2 */
#define CMD_DRIVE_DIAGS         0xe3
#define CMD_CTRL_DIAGS          0xe4
#define CMD_READ_LONG           0xe5
#define CMD_WRITE_LONG          0xe6

/* Status register (reg 1) values. */
#define STAT_REQ                0x01    // controller needs data transfer
#define STAT_IO                 0x02    // direction of transfer (TO bus)
#define STAT_CD                 0x04    // transfer of Command or Data
#define STAT_BSY                0x08    // controller is busy
#define STAT_DRQ                0x10    // DMA requested
#define STAT_IRQ                0x20    // interrupt requested
#define STAT_DCB                0x80    // not seen by driver

/* Sense Error codes. */
#define ERR_NOERROR             0x00    // no error detected
#define ERR_NOINDEX             0x01    // drive did not detect IDX pulse
#define ERR_NOSEEK              0x02    // drive did not complete SEEK
#define ERR_WRFAULT             0x03    // write fault during last cmd
#define ERR_NOTRDY              0x04    // drive did not go READY after cmd
#define ERR_NOTRK000            0x06    // drive did not see TRK0 signal
#define ERR_LONGSEEK            0x08    // long seek in progress
#define ERR_IDREAD              0x10    // ECC error during ID field
#define ERR_DATA                0x11    // uncorrectable ECC err in data
#define ERR_NOMARK              0x12    // no address mark detected
#define ERR_NOSECT              0x14    // sector not found
#define ERR_SEEK                0x15    // seek error
#define ERR_ECCDATA             0x18    // ECC corrected data
#define ERR_BADTRK              0x19    // bad track detected
#define ERR_ILLCMD              0x20    // invalid command received
#define ERR_ILLADDR             0x21    // invalid disk address received
#define ERR_BADRAM              0x30    // bad RAM in sector data buffer
#define ERR_BADROM              0x31    // bad checksum in ROM test
#define ERR_BADECC              0x32    // ECC polynomial generator bad

/* Completion Byte fields. */
#define COMP_DRIVE              0x20
#define COMP_ERR                0x02

#define IRQ_ENA                 0x02
#define DMA_ENA                 0x01


enum {
    STATE_IDLE = 0,
    STATE_RECV,
    STATE_RDATA,
    STATE_RDONE,
    STATE_SEND,
    STATE_SDATA,
    STATE_SDONE,
    STATE_COMPL
};


/* The device control block (6 bytes) */
#pragma pack(push,1)
typedef struct {
    uint8_t     cmd;                    // [7:5] class, [4:0] opcode

    uint8_t     head            :5,     // [4:0] head number
                drvsel          :1,     // [5] drive select
                mbz             :2;     // [7:6] 00

    uint8_t     sector          :6,     // [5:0] sector number 0-63
                cyl_high        :2;     // [7:6] cylinder [9:8] bits

    uint8_t     cyl_low;                // [7:0] cylinder [7:0] bits

    uint8_t     count;                  // [7:0] blk count / interleave

    uint8_t     ctrl;                   // [7:0] control field
} dcb_t;
#pragma pack(pop)

/* The (configured) Drive Parameters. */
#pragma pack(push,1)
typedef struct {
    uint8_t     cyl_high;               // (MSB) number of cylinders
    uint8_t     cyl_low;                // (LSB) number of cylinders
    uint8_t     heads;                  // number of heads per cylinder
    uint8_t     rwc_high        ;       // (MSB) reduced write current cylinder
    uint8_t     rwc_low;                // (LSB) reduced write current cylinder
    uint8_t     wp_high;                // (MSB) write precompensation cylinder
    uint8_t     wp_low;                 // (LSB) write precompensation cylinder
    uint8_t     maxecc;                 // max ECC data burst length
} dprm_t;
#pragma pack(pop)

/* Define an attached drive. */
typedef struct {
    int8_t      id,                     // drive ID on bus
                present,                // drive is present
                hdd_num,                // index to global disk table
                type;                   // drive type ID

    hdd_file_t  hdd_file;               // file with drive image

    uint16_t    cur_cyl;                // last known position of heads

    uint8_t     spt,                    // active drive parameters
                hpc;
    uint16_t    tracks;

    uint8_t     cfg_spt,                // configured drive parameters
                cfg_hpc;
    uint16_t    cfg_tracks;
} drive_t;


typedef struct {
    const char  *name;                  // controller name

    uint16_t    base;                   // controller base I/O address
    int8_t      irq;                    // controller IRQ channel
    int8_t      dma;                    // controller DMA channel
    int8_t      type;                   // controller type ID
    int8_t      spt;                    // sectors per track

    uint32_t    rom_addr;               // address where ROM is
    const char  *rom_filename;          // name of ROM image file
    rom_t       bios_rom;               // descriptor for the BIOS

    /* Controller state. */
    int8_t      state;                  // controller state
    uint8_t     sense;                  // current SENSE ERROR value
    uint8_t     status;                 // current operational status
    uint8_t     intr;

    pc_timer_t  callback_timer;

    /* Data transfer. */
    int16_t     buf_idx,                // buffer index and pointer
                buf_len;
    uint8_t     *buf_ptr;

    /* Current operation parameters. */
    dcb_t       dcb;                    // device control block
    uint16_t    track;                  // requested track#
    uint8_t     head,                   // requested head#
                sector,                 // requested sector#
                comp;                   // operation completion byte
    int         count;                  // requested sector count

    drive_t     drives[XTA_NUM];        // the attached drive(s)

    uint8_t     data[512];              // data buffer
    uint8_t     sector_buf[512];        // sector buffer
} hdc_t;


static void
set_intr(hdc_t *dev)
{
    dev->status = STAT_REQ|STAT_CD|STAT_IO|STAT_BSY;
    dev->state = STATE_COMPL;

    if (dev->intr & IRQ_ENA) {
        dev->status |= STAT_IRQ;
        picint(1 << dev->irq);
    }
}


/* Get the logical (block) address of a CHS triplet. */
static int
get_sector(hdc_t *dev, drive_t *drive, off64_t *addr)
{
    if (drive->cur_cyl != dev->track) {
        pclog("%04X:%04X %s: get_sector: wrong cylinder %i/%i\n",
              CS, cpu_state.pc, dev->name, drive->cur_cyl, dev->track);
        dev->sense = ERR_ILLADDR;
        return(0);
    }

    if (dev->head >= drive->hpc) {
        pclog("%s: get_sector: past end of heads\n", dev->name);
        dev->sense = ERR_ILLADDR;
        return(0);
    }

    if (dev->sector >= drive->spt) {
        pclog("%s: get_sector: past end of sectors\n", dev->name);
        dev->sense = ERR_ILLADDR;
        return(0);
    }

    /* Calculate logical address (block number) of desired sector. */
    *addr = ((((off64_t) dev->track*drive->hpc) + \
              dev->head)*drive->spt) + dev->sector;

    return(1);
}


static void
next_sector(hdc_t *dev, drive_t *drive)
{
    if (++dev->sector >= drive->spt) {
        dev->sector = 0;
        if (++dev->head >= drive->hpc) {
                dev->head = 0;
                dev->track++;
                if (++drive->cur_cyl >= drive->tracks)
                        drive->cur_cyl = (drive->tracks - 1);
        }
    }
}


/* Perform the seek operation. */
static void
do_seek(hdc_t *dev, drive_t *drive, int cyl)
{
    dev->track = cyl;

    if (dev->track >= drive->tracks)
        drive->cur_cyl = (drive->tracks - 1);
      else
        drive->cur_cyl = dev->track;

    if (drive->cur_cyl < 0)
        drive->cur_cyl = 0;
}


/* Format a track or an entire drive. */
static void
do_format(hdc_t *dev, drive_t *drive, dcb_t *dcb)
{
    int start_cyl, end_cyl;
    int start_hd, end_hd;
    off64_t addr;
    int h, s;

    /* Get the parameters from the DCB. */
    if (dcb->cmd == CMD_FORMAT_DRIVE) {
        start_cyl = 0;
        start_hd = 0;
        end_cyl = drive->tracks;
        end_hd = drive->hpc;
    } else {
        start_cyl = (dcb->cyl_low | (dcb->cyl_high << 8));
        start_hd = dcb->head;
        end_cyl = start_cyl + 1;
        end_hd = start_hd + 1;
    }

    switch (dev->state) {
        case STATE_IDLE:
                /* Seek to cylinder. */
                do_seek(dev, drive, start_cyl);
                dev->head = dcb->head;
                dev->sector = 0;

                pclog("%04X:%04X %s: format_%s(%i) %i,%i\n", CS, cpu_state.pc, dev->name,
                      (dcb->cmd==CMD_FORMAT_DRIVE)?"drive":"track",
                      drive->id, dev->track, dev->head);

                /* Activate the status icon. */
                // hdd_active(drive->hdd_num, 1);

do_fmt:
                /*
                 * For now, we don't use the interleave factor (in
                 * dcb->count), although we should one day use an
                 * image format that can handle it..
                 *
                 * That said, we have been given a sector_buf of
                 * data to fill the sectors with, so we will use
                 * that at least.
                 */
                for (h = start_hd; h < end_hd; h++) {
                        for (s = 0; s < drive->spt; s++) {
                                /* Set the sector we need to write. */
                                dev->head = h;
                                dev->sector = s;

                                /* Get address of sector to write. */
                                if (! get_sector(dev, drive, &addr)) break;

                                /* Write the block to the image. */
                                hdd_write_sectors(&drive->hdd_file, addr, 1,
                                                (uint8_t *)dev->sector_buf);
                        }
                }

                /* One more track done. */
                if (++start_cyl == end_cyl) break;

                /* This saves us a LOT of code. */
                goto do_fmt;
    }

    /* De-activate the status icon. */
    // hdd_active(drive->hdd_num, 0);
}


/* Execute the DCB we just received. */
static void
hdc_callback(void *priv)
{
    hdc_t *dev = (hdc_t *)priv;
    dcb_t *dcb = &dev->dcb;
    drive_t *drive;
    dprm_t *params;
    off64_t addr;
    int no_data = 0;
    int val;

    /* Get the correct drive for this request. */
    drive = &dev->drives[dcb->drvsel];
    dev->comp = (dcb->drvsel) ? COMP_DRIVE : 0x00;
    dev->status |= STAT_DCB;

    switch (dcb->cmd) {
        case CMD_TEST_READY:
                pclog("%04X:%04X %s: test_ready(%i) ready=%i\n",
                      CS, cpu_state.pc, dev->name, dcb->drvsel, drive->present);

                if (! drive->present) {
                        dev->comp |= COMP_ERR;
                        dev->sense = ERR_NOTRDY;
                }
                set_intr(dev);
                break;

        case CMD_RECALIBRATE:
                pclog("%04X:%04X %s: recalibrate(%i) ready=%i\n",
                      CS, cpu_state.pc, dev->name, dcb->drvsel, drive->present);

                if (! drive->present) {
                        dev->comp |= COMP_ERR;
                        dev->sense = ERR_NOTRDY;
                } else {
                        dev->track = drive->cur_cyl = 0;
                }
                set_intr(dev);
                break;
                
        case CMD_READ_SENSE:
                switch (dev->state) {
                        case STATE_IDLE:
                                pclog("%04X:%04X %s: sense(%i)\n",
                                      CS, cpu_state.pc, dev->name, dcb->drvsel);

                                dev->buf_idx = 0;
                                dev->buf_len = 4;
                                dev->buf_ptr = dev->data;
                                dev->buf_ptr[0] = dev->sense;
                                dev->buf_ptr[1] = dcb->drvsel ? 0x20 : 0x00;
                                dev->buf_ptr[2] = (drive->cur_cyl >> 2) | \
                                                  (dev->sector & 0x3f);
                                dev->buf_ptr[3] = (drive->cur_cyl & 0xff);
                                dev->sense = ERR_NOERROR;
                                dev->status |= (STAT_IO | STAT_REQ);
                                dev->state = STATE_SDATA;
                                break;

                        case STATE_SDONE:
                                set_intr(dev);
                }
                break;          

        case CMD_READ_VERIFY:
                no_data = 1;
                /*FALLTHROUGH*/
        case CMD_READ_SECTORS:
                if (! drive->present) {
                        dev->comp |= COMP_ERR;
                        dev->sense = ERR_NOTRDY;
                        set_intr(dev);
                        break;
                }

                switch (dev->state) {
                        case STATE_IDLE:
                                /* Seek to cylinder. */
                                do_seek(dev, drive,
                                        (dcb->cyl_low|(dcb->cyl_high<<8)));
                                dev->head = dcb->head;
                                dev->sector = dcb->sector;

                                /* Get sector count; count=0 means 256. */
                                dev->count = (int)dcb->count;
                                if (dev->count == 0)
                                        dev->count = 256;
                                dev->buf_len = 512;

                                dev->state = STATE_SEND;
                                /*FALLTHROUGH*/

                        case STATE_SEND:
                                /* Activate the status icon. */
                                // hdd_active(drive->hdd_num, 1);

                                pclog("%04X:%04X %s: read_%s(%i: %i,%i,%i) cnt=%i\n",
                                      CS, cpu_state.pc, dev->name, (no_data)?"verify":"sector",
                                      drive->id, dev->track, dev->head,
                                      dev->sector, dev->count);
do_send:
                                /* Get address of sector to load. */
                                if (! get_sector(dev, drive, &addr)) {
                                        /* De-activate the status icon. */
                                        // hdd_active(drive->hdd_num, 0);
                                        dev->comp |= COMP_ERR;
                                        set_intr(dev);
                                        return;
                                }

                                /* Read the block from the image. */
                                hdd_read_sectors(&drive->hdd_file, addr, 1,
                                               (uint8_t *)dev->sector_buf);

                                /* Ready to transfer the data out. */
                                dev->state = STATE_SDATA;
                                dev->buf_idx = 0;
                                if (no_data) {
                                        /* Delay a bit, no actual transfer. */
                                        timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                } else {
                                        if (dev->intr & DMA_ENA) {
                                                /* DMA enabled. */
                                                dev->buf_ptr = dev->sector_buf;
                                                timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                        } else {
                                                /* Copy from sector to data. */
                                                memcpy(dev->data,
                                                       dev->sector_buf,
                                                       dev->buf_len);
                                                dev->buf_ptr = dev->data;

                                                dev->status |= (STAT_IO | STAT_REQ);
                                        }
                                }
                                break;
                        
                        case STATE_SDATA:
                                if (! no_data) {
                                        /* Perform DMA. */
                                        while (dev->buf_idx < dev->buf_len) {
                                                val = dma_channel_write(dev->dma,
                                                        *dev->buf_ptr);
                                                if (val == DMA_NODATA) {
                                                        fatal("%04X:%04X %s: CMD_READ_SECTORS out of data (idx=%i, len=%i)!\n",
                                                                CS, cpu_state.pc, dev->name, dev->buf_idx, dev->buf_len);

                                                        dev->status |= (STAT_CD | STAT_IO| STAT_REQ);
                                                        timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                                        return;
                                                }
                                                dev->buf_ptr++;
                                                dev->buf_idx++;
                                        }
                                }
                                timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                dev->state = STATE_SDONE;
                                break;

                        case STATE_SDONE:
                                dev->buf_idx = 0;
                                if (--dev->count == 0) {
                                        pclog("%04X:%04X %s: read_%s(%i) DONE\n",
                                              CS, cpu_state.pc, dev->name,
                                              (no_data)?"verify":"sector",
                                              drive->id);

                                        /* De-activate the status icon. */
                                        // hdd_active(drive->hdd_num, 0);

                                        set_intr(dev);
                                        return;
                                }

                                /* Addvance to next sector. */
                                next_sector(dev, drive);

                                /* This saves us a LOT of code. */
                                dev->state = STATE_SEND;
                                goto do_send;
                }
                break;

#ifdef CMD_WRITE_VERIFY
        case CMD_WRITE_VERIFY:
                no_data = 1;
                /*FALLTHROUGH*/
#endif
        case CMD_WRITE_SECTORS:
                if (! drive->present) {
                        dev->comp |= COMP_ERR;
                        dev->sense = ERR_NOTRDY;
                        set_intr(dev);
                        break;
                }

                switch (dev->state) {
                        case STATE_IDLE:
                                /* Seek to cylinder. */
                                do_seek(dev, drive,
                                        (dcb->cyl_low|(dcb->cyl_high<<8)));
                                dev->head = dcb->head;
                                dev->sector = dcb->sector;

                                /* Get sector count; count=0 means 256. */
                                dev->count = (int)dev->dcb.count;
                                if (dev->count == 0)
                                        dev->count = 256;
                                dev->buf_len = 512;

                                dev->state = STATE_RECV;
                                /*FALLTHROUGH*/

                        case STATE_RECV:
                                /* Activate the status icon. */
                                // hdd_active(drive->hdd_num, 1);

                                pclog("%04X:%04X %s: write_%s(%i: %i,%i,%i) cnt=%i\n",
                                      CS, cpu_state.pc, dev->name, (no_data)?"verify":"sector",
                                      dcb->drvsel, dev->track,
                                      dev->head, dev->sector, dev->count);
do_recv:
                                /* Ready to transfer the data in. */
                                dev->state = STATE_RDATA;
                                dev->buf_idx = 0;
                                if (no_data) {
                                        /* Delay a bit, no actual transfer. */
                                        timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                } else {
                                        if (dev->intr & DMA_ENA) {
                                                /* DMA enabled. */
                                                dev->buf_ptr = dev->sector_buf;
                                                timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                        } else {
                                                /* No DMA, do PIO. */
                                                dev->buf_ptr = dev->data;
                                                dev->status |= STAT_REQ;
                                        }
                                }
                                break;

                        case STATE_RDATA:
                                if (! no_data) {
                                        /* Perform DMA. */
                                        dev->status = STAT_BSY;
                                        while (dev->buf_idx < dev->buf_len) {
                                                val = dma_channel_read(dev->dma);
                                                if (val == DMA_NODATA) {
                                                        fatal("%04X:%04X %s: CMD_WRITE_SECTORS out of data (idx=%i, len=%i)!\n",
                                                                CS, cpu_state.pc, dev->name, dev->buf_idx, dev->buf_len);

                                                        dev->status |= (STAT_CD | STAT_IO | STAT_REQ);
                                                        timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                                        return;
                                                }

                                                dev->buf_ptr[dev->buf_idx] = (val & 0xff);
                                                dev->buf_idx++;
                                        }
                                        dev->state = STATE_RDONE;
                                        timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                }
                                break;

                        case STATE_RDONE:
                                /* Copy from data to sector if PIO. */
                                if (! (dev->intr & DMA_ENA))
                                        memcpy(dev->sector_buf, dev->data,
                                               dev->buf_len);

                                /* Get address of sector to write. */
                                if (! get_sector(dev, drive, &addr)) {
                                        /* De-activate the status icon. */
                                        // hdd_active(drive->hdd_num, 0);

                                        dev->comp |= COMP_ERR;
                                        set_intr(dev);
                                        return;
                                }

                                /* Write the block to the image. */
                                hdd_write_sectors(&drive->hdd_file, addr, 1,
                                                (uint8_t *)dev->sector_buf);

                                dev->buf_idx = 0;
                                if (--dev->count == 0) {
                                        pclog("%04X:%04X %s: write_%s(%i) DONE\n",
                                              CS, cpu_state.pc, dev->name,
                                              (no_data)?"verify":"sector",
                                              drive->id);

                                        /* De-activate the status icon. */
                                        // hdd_active(drive->hdd_num, 0);

                                        set_intr(dev);
                                        return;
                                }

                                /* Advance to next sector. */
                                next_sector(dev, drive);

                                /* This saves us a LOT of code. */
                                dev->state = STATE_RECV;
                                goto do_recv;
                }
                break;

        case CMD_FORMAT_DRIVE:
        case CMD_FORMAT_TRACK:
                if (drive->present) {
                        do_format(dev, drive, dcb);
                } else {
                        dev->comp |= COMP_ERR;
                        dev->sense = ERR_NOTRDY;
                }
                set_intr(dev);
                break;

        case CMD_SEEK:
                /* Seek to cylinder. */
                val = (dcb->cyl_low | (dcb->cyl_high << 8));

                pclog("%04X:%04X %s: seek(%i) %i/%i ready=%i\n", CS, cpu_state.pc, dev->name,
                      dcb->drvsel, val, drive->cur_cyl, drive->present);

                if (drive->present) {
                        do_seek(dev, drive, val);
                        if (val != drive->cur_cyl) {
                                dev->comp |= COMP_ERR;
                                dev->sense = ERR_SEEK;
                        }
                } else {
                        dev->comp |= COMP_ERR;
                        dev->sense = ERR_NOTRDY;
                }
                set_intr(dev);
                break;

        case CMD_SET_DRIVE_PARAMS:
                switch (dev->state) {
                        case STATE_IDLE:
                                dev->state = STATE_RDATA;
                                dev->buf_idx = 0;
                                dev->buf_len = sizeof(dprm_t);
                                dev->buf_ptr = (uint8_t *)dev->data;
                                dev->status |= STAT_REQ;
                                break;

                        case STATE_RDONE:
                                params = (dprm_t *)dev->data;
                                drive->tracks =
                                    (params->cyl_high << 8) | params->cyl_low;
                                drive->hpc = params->heads;
                                drive->spt = dev->spt;  /*hardcoded*/
//FIXME: not correct
                                pclog("%04X:%04X %s: set_params(%i) cyl=%i,hd=%i,spt=%i\n",
                                      CS, cpu_state.pc, dev->name, dcb->drvsel, drive->tracks,
                                      drive->hpc, drive->spt);

                                dev->status &= ~STAT_REQ;
                                set_intr(dev);
                                break;
                }
                break;

        case CMD_WRITE_SECTOR_BUFFER:
                switch (dev->state) {
                        case STATE_IDLE:
                                pclog("%s: write_sector_buffer()\n", dev->name);
                                dev->buf_idx = 0;
                                dev->buf_len = 512;
                                dev->state = STATE_RDATA;
                                if (dev->intr & DMA_ENA) {
                                        dev->buf_ptr = dev->sector_buf;
                                        timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                } else {
                                        dev->buf_ptr = dev->data;
                                        dev->status |= STAT_REQ;
                                }
                                break;

                        case STATE_RDATA:
                                if (dev->intr & DMA_ENA) {
                                        /* Perform DMA. */
                                        while (dev->buf_idx < dev->buf_len) {
                                                val = dma_channel_read(dev->dma);
                                                if (val == DMA_NODATA) {
                                                        fatal("%s: CMD_WRITE_BUFFER out of data!\n",
                                                                dev->name);
                                                        dev->status |= (STAT_CD | STAT_IO | STAT_REQ);
                                                        timer_set_delay_u64(&dev->callback_timer, HDC_TIME);;
                                                        return;
                                                }

                                                dev->buf_ptr[dev->buf_idx] = (val & 0xff);
                                                dev->buf_idx++;
                                        }
                                        dev->state = STATE_RDONE;
                                        timer_set_delay_u64(&dev->callback_timer, HDC_TIME);
                                }
                                break;

                        case STATE_RDONE:
                                if (! (dev->intr & DMA_ENA))
                                        memcpy(dev->sector_buf,
                                               dev->data, dev->buf_len);
                                set_intr(dev);
                                break;
                }
                break;

        case CMD_RAM_DIAGS:
                switch (dev->state) {
                        case STATE_IDLE:
                                pclog("%s: ram_diags\n", dev->name);
                                dev->state = STATE_RDONE;
                                timer_set_delay_u64(&dev->callback_timer, 5*HDC_TIME);
                                break;

                        case STATE_RDONE:
                                set_intr(dev);
                                break;
                }
                break;

        case CMD_DRIVE_DIAGS:
                switch (dev->state) {
                        case STATE_IDLE:
                                pclog("%04X:%04X %s: drive_diags(%i) ready=%i\n",
                                      CS, cpu_state.pc, dev->name, dcb->drvsel, drive->present);

                                if (drive->present) {
                                        dev->state = STATE_RDONE;
                                        timer_set_delay_u64(&dev->callback_timer, 5*HDC_TIME);
                                } else {
                                        dev->comp |= COMP_ERR;
                                        dev->sense = ERR_NOTRDY;
                                        set_intr(dev);
                                }
                                break;

                        case STATE_RDONE:
                                set_intr(dev);
                                break;
                }
                break;

        case CMD_CTRL_DIAGS:
                switch (dev->state) {
                        case STATE_IDLE:
                                pclog("%s: ctrl_diags\n", dev->name);
                                dev->state = STATE_RDONE;
                                timer_set_delay_u64(&dev->callback_timer, 5*HDC_TIME);
                                break;

                        case STATE_RDONE:
                                set_intr(dev);
                                break;
                }
                break;

        default:
                pclog("%04X:%04X %s: unknown command - %02x\n", CS, cpu_state.pc, dev->name, dcb->cmd);
                dev->comp |= COMP_ERR;
                dev->sense = ERR_ILLCMD;
                set_intr(dev);
    }
}


/* Read one of the controller registers. */
static uint8_t
hdc_in(uint16_t port, void *priv)
{
    hdc_t *dev = (hdc_t *)priv;
    uint8_t ret = 0xff;
        
    switch (port & 7) {
        case 0:         /* DATA register */
                dev->status &= ~STAT_IRQ;

                if (dev->state == STATE_SDATA) {
                        if (dev->buf_idx > dev->buf_len) {
                                pclog("%s: read with empty buffer!\n",
                                                        dev->name);
                                dev->comp |= COMP_ERR;
                                dev->sense = ERR_ILLCMD;
                                break;
                        }

                        ret = dev->buf_ptr[dev->buf_idx];
                        if (++dev->buf_idx == dev->buf_len) {
                                /* All data sent. */
                                dev->status &= ~STAT_REQ;
                                dev->state = STATE_SDONE;
                                timer_set_delay_u64(&dev->callback_timer, 5*HDC_TIME);
                        }
                } else if (dev->state == STATE_COMPL) {
                        ret = dev->comp;
                        dev->status = 0x00;
                        dev->state = STATE_IDLE;
                }
                break;

        case 1:         /* STATUS register */
                ret = (dev->status & ~STAT_DCB);
                break;

        case 2:         /* "read option jumpers" */
                ret = 0x0;             /*  0xff: all switches off */
                break;
    }

    // pclog("%04X:%04X %s: reading port=%04x val=%02x\n", CS, cpu_state.pc, dev->name, port, ret);
    return(ret);        
}


/* Write to one of the controller registers. */
static void
hdc_out(uint16_t port, uint8_t val, void *priv)
{
    hdc_t *dev = (hdc_t *)priv;

    // pclog("%04X:%04X %s: writing port=%04x val=%02x\n", CS, cpu_state.pc, dev->name, port, val);

    switch (port & 7) {
        case 0:         /* DATA register */
                if (dev->state == STATE_RDATA) {
                        if (! (dev->status & STAT_REQ)) {
                                pclog("%s: not ready for command/data!\n",
                                                                dev->name);
                                dev->comp |= COMP_ERR;
                                dev->sense = ERR_ILLCMD;
                                break;
                        }

                        if (dev->buf_idx >= dev->buf_len) {
                                pclog("%s: write with full buffer!\n",
                                                        dev->name);
                                dev->comp |= COMP_ERR;
                                dev->sense = ERR_ILLCMD;
                                break;
                        }

                        /* Store the data into the buffer. */
                        dev->buf_ptr[dev->buf_idx] = val;
                        if (++dev->buf_idx == dev->buf_len) {
                                /* We got all the data we need. */
                                dev->status &= ~STAT_REQ;
                                if (dev->status & STAT_DCB)
                                        dev->state = STATE_RDONE;
                                  else
                                        dev->state = STATE_IDLE;
                                dev->status &= ~STAT_CD;
                                timer_set_delay_u64(&dev->callback_timer, 5*HDC_TIME);
                        }
                }
                break;

        case 1:         /* RESET register */
                dev->sense = 0x00;
                dev->state = STATE_IDLE;
                break;

        case 2:         /* "controller-select" */
                /* Reset the DCB buffer. */
                dev->buf_idx = 0;
                dev->buf_len = sizeof(dcb_t);
                dev->buf_ptr = (uint8_t *)&dev->dcb;
                dev->state = STATE_RDATA;
                dev->status = (STAT_BSY | STAT_CD | STAT_REQ);
                break;

        case 3:         /* DMA/IRQ intr register */
                dev->intr = val;
                break;
    }
}


static void
xta_close(void *priv)
{
    hdc_t *dev = (hdc_t *)priv;
    drive_t *drive;
    int d;

    /* Remove the I/O handler. */
    io_removehandler(dev->base, 4,
                     hdc_in,NULL,NULL, hdc_out,NULL,NULL, dev);

    /* Close all disks and their images. */
    for (d = 0; d < XTA_NUM; d++) {
        drive = &dev->drives[d];

        hdd_close(&drive->hdd_file);
    }

    /* Release the device. */
    free(dev);
}


static void *xta_init(int8_t type)
// const device_t *info, UNUSED(void *parent))
{
    drive_t *drive;
    hdc_t *dev;
    int c, i;
    int bus;

    /* Allocate and initialize device block. */
    dev = (hdc_t *)malloc(sizeof(hdc_t));
    memset(dev, 0x00, sizeof(hdc_t));
    dev->type = type;
    // bus = (info->local >> 8) & 255;

    /* Do per-controller-type setup. */
    switch (dev->type) {

        case 0:         // WDXT-150, with BIOS
                dev->name = "WDXT-150";
                dev->base = device_get_config_int("base");
                dev->irq = device_get_config_int("irq");
                dev->rom_addr = device_get_config_int("bios_addr");
                dev->rom_filename = WD_BIOS_FILE;
                dev->dma = 3;
                dev->spt = 17;          // MFM
                break;

        case 1:         // EuroPC
                dev->name = "HD20";
                dev->base = 0x0320;
                dev->irq = 5;
                dev->dma = 3;
                dev->spt = 17;          // MFM
                break;

        case 2:         // Toshiba T1200
                dev->name = "T1200-HD";
                dev->base = 0x0320;
                dev->irq = 5;
                dev->dma = 3;
                // dev->rom_addr = 0xc8000;
                // dev->rom_filename = WD_BIOS_FILE;
                dev->spt = 34;
                break;

        case 3:         // Amstrad PC5086
                dev->name = "PC5086-HD";
                dev->base = 0x0320;
                dev->irq = 5;
                dev->dma = 3;
                dev->rom_addr = 0xc8000;
                dev->rom_filename = PC5086_BIOS_FILE;
                dev->spt = 17;          // MFM
                break;
    }

    pclog("%04X:%04X %s: initializing (I/O=%04X, IRQ=%i, DMA=%i",
                CS, cpu_state.pc, dev->name, dev->base, dev->irq, dev->dma);
    if (dev->rom_addr != 0x000000)
        pclog(", BIOS=%06X", dev->rom_addr);
    pclog(")\n");

    /* Load any disks for this device class. */
    for (i = 0; i < XTA_NUM; i++) {
        drive = &dev->drives[i];

        hdd_load(&drive->hdd_file, i, ide_fn[i] );
        if (drive->hdd_file.f == NULL)  {
                drive->present = 0;
                continue;
        }
        drive->id = i;
        drive->hdd_num = i;
        drive->present = 1;

        /* These are the "hardware" parameters (from the image.) */
        drive->spt = (uint8_t)(drive->hdd_file.spt & 0xff);
        drive->hpc = (uint8_t)(drive->hdd_file.hpc & 0xff);
        drive->tracks = (uint16_t)drive->hdd_file.tracks;

        /* Use them as "configured" parameters until overwritten. */
        drive->cfg_spt = drive->spt;
        drive->cfg_hpc = drive->hpc;
        drive->cfg_tracks = drive->tracks;

        pclog("%04X:%04X %s: drive%i (cyl=%i,hd=%i,spt=%i), disk %i\n",
             CS, cpu_state.pc, dev->name, i,
             drive->tracks, drive->hpc, drive->spt, i);

    }

    /* Enable the I/O block. */
    io_sethandler(dev->base, 4,
                  hdc_in,NULL,NULL, hdc_out,NULL,NULL, dev);

    /* Load BIOS if it has one. */
    if (dev->rom_addr != 0x000000)
        rom_init(&dev->bios_rom, (char *)dev->rom_filename,
                 dev->rom_addr, 0x4000, 0x3fff, 0, MEM_MAPPING_EXTERNAL);
                
    /* Create a timer for command delays. */
    timer_add(&dev->callback_timer, hdc_callback, dev, 0);

    return((void *)dev);
}


static const device_config_t wdxt150_config[] = {
    {
            .name = "base",
            .description = "Address",
            .type = CONFIG_SELECTION,
            .selection =
            {
                    {
                            .description = "320H",
                            .value = 0x320
                    },
                    {
                            .description = "340H",
                            .value = 0x340
                    }
            },
            .default_int = 0x320
    },
    {
            .name = "irq",
            .description = "IRQ",
            .type = CONFIG_SELECTION,
            .selection =
            {
                    {
                            .description = "IRQ 5",
                            .value = 5
                    },
                    {
                            .description = "IRQ 4",
                            .value = 4
                    }
            },
            .default_int = 5
    },
    {
            .name = "bios_addr",
            .description = "BIOS Address",
            .type = CONFIG_SELECTION,
            .selection =
            {
                    {
                            .description = "C800H",
                            .value = 0xc8000
                    },
                    {
                            .description = "CA00H",
                            .value = 0xca000
                    }
            },
            .default_int = 0xc8000
    },
    {
        .type = -1
    }
};

static void *xta_wdxt150_init() {
    return xta_init(0);
}

static int xta_wdxt150_available()
{
        return rom_present(WD_BIOS_FILE);
}

device_t xta_wdxt150_device = {
    "WDXT-150 XTA Fixed Disk Controller",
    0,
    xta_wdxt150_init, 
    xta_close, 
    xta_wdxt150_available,
    NULL, 
    NULL, 
    NULL, 
    NULL,
};

static void *xta_hd20_init() {
    return xta_init(1);
}

device_t xta_hd20_device = {
    "EuroPC HD20 Fixed Disk Controller",
    0,
    xta_hd20_init, 
    xta_close, 
    NULL,
    NULL, 
    NULL, 
    NULL, 
    NULL,
};

static void *xta_t1200_init() {
    return xta_init(2);
}

device_t xta_t1200_device = {
    "Toshiba T1200 Fixed Disk Controller",
    0,
    xta_t1200_init, 
    xta_close, 
    NULL,
    NULL, 
    NULL, 
    NULL, 
    NULL,
};

static void *xta_pc5086_init() {
    return xta_init(3);
}

static int xta_pc5086_available()
{
        return rom_present(PC5086_BIOS_FILE);
}

device_t xta_pc5086_device = {
    "Amstrad PC5086 Fixed Disk Controller",
    0,
    xta_pc5086_init, 
    xta_close, 
    xta_pc5086_available,
    NULL, 
    NULL, 
    NULL, 
    NULL,
};
