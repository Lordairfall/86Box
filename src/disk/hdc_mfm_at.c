/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Driver for the IBM PC-AT MFM/RLL Fixed Disk controller.
 *
 *		This controller was a 16bit ISA card, and it used a WD1003
 *		based design. Most cards were WD1003-WA2 or -WAH, where the
 *		-WA2 cards had a floppy controller as well (to save space.)
 *
 * Version:	@(#)hdc_mfm_at.c	1.0.17	2018/05/02
 *
 * Authors:	Sarah Walker, <http://pcem-emulator.co.uk/>
 *		Fred N. van Kempen, <decwiz@yahoo.com>
 *
 *		Copyright 2008-2018 Sarah Walker.
 *		Copyright 2017,2018 Fred N. van Kempen.
 */
#define __USE_LARGEFILE64
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include "../86box.h"
#include "../device.h"
#include "../io.h"
#include "../pic.h"
#include "../cpu/cpu.h"
#include "../machine/machine.h"
#include "../timer.h"
#include "../plat.h"
#include "../ui.h"
#include "hdc.h"
#include "hdd.h"


#define MFM_TIME		(TIMER_USEC*10LL)

#define STAT_ERR		0x01
#define STAT_INDEX		0x02
#define STAT_ECC		0x04
#define STAT_DRQ		0x08	/* data request */
#define STAT_DSC		0x10
#define STAT_WRFLT		0x20
#define STAT_READY		0x40
#define STAT_BUSY		0x80

#define ERR_DAM_NOT_FOUND	0x01	/* Data Address Mark not found */
#define ERR_TR000		0x02	/* track 0 not found */
#define ERR_ABRT		0x04	/* command aborted */
#define ERR_ID_NOT_FOUND	0x10	/* ID not found */
#define ERR_DATA_CRC		0x40	/* data CRC error */
#define ERR_BAD_BLOCK		0x80	/* bad block detected */

#define CMD_RESTORE		0x10
#define CMD_READ		0x20
#define CMD_WRITE		0x30
#define CMD_VERIFY		0x40
#define CMD_FORMAT		0x50
#define CMD_SEEK		0x70
#define CMD_DIAGNOSE		0x90
#define CMD_SET_PARAMETERS	0x91


typedef struct {
    int8_t	present,		/* drive is present */
		hdd_num,		/* drive number in system */
		steprate,		/* current servo step rate */
		spt,			/* physical #sectors per track */
		hpc,			/* physical #heads per cylinder */
		pad;
    int16_t	tracks;			/* physical #tracks per cylinder */

    int8_t	cfg_spt,		/* configured #sectors per track */
		cfg_hpc;		/* configured #heads per track */

    int16_t	curcyl;			/* current track number */
} drive_t;


typedef struct {
    uint8_t	precomp,		/* 1: precomp/error register */
		error,
		secount,		/* 2: sector count register */
		sector,			/* 3: sector number */
		head,			/* 6: head number + drive select */
		command,		/* 7: command/status */
		status,
		fdisk;			/* 8: control register */
    uint16_t	cylinder;		/* 4/5: cylinder LOW and HIGH */

    int8_t	reset,			/* controller in reset */
		irqstat,		/* current IRQ status */
		drvsel,			/* current selected drive */
		pad;

    int		pos;			/* offset within data buffer */
    int64_t	callback;		/* callback delay timer */

    uint16_t	buffer[256];		/* data buffer (16b wide) */

    drive_t	drives[MFM_NUM];	/* attached drives */
} mfm_t;


#ifdef ENABLE_MFM_AT_LOG
int mfm_at_do_log = ENABLE_MFM_AT_LOG;
#endif


static void
mfm_at_log(const char *fmt, ...)
{
#ifdef ENABLE_MFM_AT_LOG
    va_list ap;

    if (mfm_at_do_log) {
	va_start(ap, fmt);
	pclog_ex(fmt, ap);
	va_end(ap);
    }
#endif
}


static inline void
irq_raise(mfm_t *mfm)
{
    if (!(mfm->fdisk & 2))
	picint(1 << 14);

    mfm->irqstat = 1;
}


static inline void
irq_lower(mfm_t *mfm)
{
    if (mfm->irqstat) {
	if (!(mfm->fdisk & 2))
		picintc(1 << 14);

	mfm->irqstat = 0;
    }
}


/*
 * Return the sector offset for the current register values.
 *
 * According to the WD1002/WD1003 technical reference manual,
 * this is not done entirely correct. It specifies that the
 * parameters set with the SET_DRIVE_PARAMETERS command are
 * to be used only for multi-sector operations, and that any
 * such operation can only be executed AFTER these parameters
 * have been set. This would imply that for regular single
 * transfers, the controller uses (or, can use) the actual
 * geometry information...
 */
static int
get_sector(mfm_t *mfm, off64_t *addr)
{
    drive_t *drive = &mfm->drives[mfm->drvsel];

    if (drive->curcyl != mfm->cylinder) {
	mfm_at_log("WD1003(%d) sector: wrong cylinder\n");
	return(1);
    }

    if (mfm->head > drive->cfg_hpc) {
	mfm_at_log("WD1003(%d) get_sector: past end of configured heads\n",
							mfm->drvsel);
	return(1);
    }

    if (mfm->sector >= drive->cfg_spt+1) {
	mfm_at_log("WD1003(%d) get_sector: past end of configured sectors\n",
							mfm->drvsel);
	return(1);
    }

#if 1
    /* We should check this in the SET_DRIVE_PARAMETERS command!  --FvK */
    if (mfm->head > drive->hpc) {
	mfm_at_log("WD1003(%d) get_sector: past end of heads\n", mfm->drvsel);
	return(1);
    }

    if (mfm->sector >= drive->spt+1) {
	mfm_at_log("WD1003(%d) get_sector: past end of sectors\n", mfm->drvsel);
	return(1);
    }
#endif

    *addr = ((((off64_t) mfm->cylinder * drive->cfg_hpc) + mfm->head) *
			 drive->cfg_spt) + (mfm->sector - 1);

    return(0);
}


/* Move to the next sector using CHS addressing. */
static void
next_sector(mfm_t *mfm)
{
    drive_t *drive = &mfm->drives[mfm->drvsel];

    if (++mfm->sector == (drive->cfg_spt+1)) {
	mfm->sector = 1;
	if (++mfm->head == drive->cfg_hpc) {
		mfm->head = 0;
		mfm->cylinder++;
		if (drive->curcyl < drive->tracks)
			drive->curcyl++;
	}
    }
}


static void
mfm_cmd(mfm_t *mfm, uint8_t val)
{
    drive_t *drive = &mfm->drives[mfm->drvsel];

    if (! drive->present) {
	/* This happens if sofware polls all drives. */
	mfm_at_log("WD1003(%d) command %02x on non-present drive\n",
					mfm->drvsel, val);
	mfm->command = 0xff;
	mfm->status = STAT_BUSY;
	timer_clock();
	mfm->callback = 200LL*MFM_TIME;
	timer_update_outstanding();

	return;
    }

    irq_lower(mfm);
    mfm->command = val;
    mfm->error = 0;

    switch (val & 0xf0) {
	case CMD_RESTORE:
		drive->steprate = (val & 0x0f);
		mfm_at_log("WD1003(%d) restore, step=%d\n",
			mfm->drvsel, drive->steprate);
		drive->curcyl = 0;
		mfm->status = STAT_READY|STAT_DSC;
		mfm->command &= 0xf0;
		irq_raise(mfm);
		break;

	case CMD_SEEK:
		drive->steprate = (val & 0x0f);
		mfm->command &= 0xf0;
		mfm->status = STAT_BUSY;
		timer_clock();
		mfm->callback = 200LL*MFM_TIME;
		timer_update_outstanding();
		break;

	default:
		mfm->command = val;
		switch (val) {
			case CMD_READ:
			case CMD_READ+1:
			case CMD_READ+2:
			case CMD_READ+3:
				mfm_at_log("WD1003(%d) read, opt=%d\n",
					mfm->drvsel, val&0x03);
				mfm->command &= 0xfc;
				if (val & 2)
					fatal("WD1003: READ with ECC\n");
				mfm->status = STAT_BUSY;
				timer_clock();
				mfm->callback = 200LL*MFM_TIME;
				timer_update_outstanding();
				break;

			case CMD_WRITE:
			case CMD_WRITE+1:
			case CMD_WRITE+2:
			case CMD_WRITE+3:
				mfm_at_log("WD1003(%d) write, opt=%d\n",
					mfm->drvsel, val & 0x03);
				mfm->command &= 0xfc;
				if (val & 2)
					fatal("WD1003: WRITE with ECC\n");
				mfm->status = STAT_DRQ|STAT_DSC;
				mfm->pos = 0;
				break;

			case CMD_VERIFY:
			case CMD_VERIFY+1:
				mfm->command &= 0xfe;
				mfm->status = STAT_BUSY;
				timer_clock();
				mfm->callback = 200LL*MFM_TIME;
				timer_update_outstanding();
				break;

			case CMD_FORMAT:
				mfm->status = STAT_DRQ|STAT_BUSY;
				mfm->pos = 0;
				break;

			case CMD_DIAGNOSE:
				mfm->status = STAT_BUSY;
				timer_clock();
				mfm->callback = 200LL*MFM_TIME;
				timer_update_outstanding();
				break;

			case CMD_SET_PARAMETERS:
				/*
				 * NOTE:
				 *
				 * We currently just set these parameters, and
				 * never bother to check if they "fit within"
				 * the actual parameters, as determined by the
				 * image loader.
				 *
				 * The difference in parameters is OK, and
				 * occurs when the BIOS or operating system
				 * decides to use a different translation
				 * scheme, but either way, it SHOULD always
				 * fit within the actual parameters!
				 *
				 * We SHOULD check that here!! --FvK
				 */
				if (drive->cfg_spt == 0) {
					/* Only accept after RESET or DIAG. */
					drive->cfg_spt = mfm->secount;
					drive->cfg_hpc = mfm->head+1;
					mfm_at_log("WD1003(%d) parameters: tracks=%d, spt=%i, hpc=%i\n",
						mfm->drvsel, drive->tracks,
						drive->cfg_spt, drive->cfg_hpc);
				} else {
					mfm_at_log("WD1003(%d) parameters: tracks=%d,spt=%i,hpc=%i (IGNORED)\n",
						mfm->drvsel, drive->tracks,
						drive->cfg_spt, drive->cfg_hpc);
				}
				mfm->command = 0x00;
				mfm->status = STAT_READY|STAT_DSC;
				mfm->error = 1;
				irq_raise(mfm);
				break;

			default:
				mfm_at_log("WD1003: bad command %02X\n", val);
				mfm->status = STAT_BUSY;
				timer_clock();
				mfm->callback = 200LL*MFM_TIME;
				timer_update_outstanding();
				break;
		}
    }
}


static void
mfm_writew(uint16_t port, uint16_t val, void *priv)
{
    mfm_t *mfm = (mfm_t *)priv;

    mfm->buffer[mfm->pos >> 1] = val;
    mfm->pos += 2;

    if (mfm->pos >= 512) {
	mfm->pos = 0;
	mfm->status = STAT_BUSY;
	timer_clock();
	/* 781.25 us per sector at 5 Mbit/s = 640 kB/s. */
	mfm->callback = ((3125LL * TIMER_USEC) / 4LL);
	/* mfm->callback = 10LL * MFM_TIME; */
	timer_update_outstanding();
    }
}


static void
mfm_write(uint16_t port, uint8_t val, void *priv)
{
    mfm_t *mfm = (mfm_t *)priv;

    mfm_at_log("WD1003 write(%04x, %02x)\n", port, val);

    switch (port) {
	case 0x01f0:	/* data */
		mfm_writew(port, val | (val << 8), priv);
		return;

	case 0x01f1:	/* write precompenstation */
		mfm->precomp = val;
		return;

	case 0x01f2:	/* sector count */
		mfm->secount = val;
		return;

	case 0x01f3:	/* sector */
		mfm->sector = val;
		return;

	case 0x01f4:	/* cylinder low */
		mfm->cylinder = (mfm->cylinder & 0xff00) | val;
		return;

	case 0x01f5:	/* cylinder high */
		mfm->cylinder = (mfm->cylinder & 0xff) | (val << 8);
		return;

	case 0x01f6:	/* drive/head */
		mfm->head = val & 0xF;
		mfm->drvsel = (val & 0x10) ? 1 : 0;
		if (mfm->drives[mfm->drvsel].present)
			mfm->status = STAT_READY|STAT_DSC;
		else
			mfm->status = 0;
		return;

	case 0x01f7:	/* command register */
		mfm_cmd(mfm, val);
		break;

	case 0x03f6:	/* device control */
		val &= 0x0f;
		if ((mfm->fdisk & 0x04) && !(val & 0x04)) {
			timer_clock();
			mfm->callback = 500LL*MFM_TIME;
			timer_update_outstanding();
			mfm->reset = 1;
			mfm->status = STAT_BUSY;
		}

		if (val & 0x04) {
			/* Drive held in reset. */
			timer_clock();
			mfm->callback = 0LL;
			timer_update_outstanding();
			mfm->status = STAT_BUSY;
		}
		mfm->fdisk = val;
		/* Lower IRQ on IRQ disable. */
		if ((val & 2) && !(mfm->fdisk & 0x02))
			picintc(1 << 14);
		break;
    }
}


static uint16_t
mfm_readw(uint16_t port, void *priv)
{
    mfm_t *mfm = (mfm_t *)priv;
    uint16_t ret;

    ret = mfm->buffer[mfm->pos >> 1];
    mfm->pos += 2;
    if (mfm->pos >= 512) {
	mfm->pos = 0;
	mfm->status = STAT_READY|STAT_DSC;
	if (mfm->command == CMD_READ) {
		mfm->secount = (mfm->secount - 1) & 0xff;
		if (mfm->secount) {
			next_sector(mfm);
			mfm->status = STAT_BUSY;
			timer_clock();
			/* 781.25 us per sector at 5 Mbit/s = 640 kB/s. */
			mfm->callback = ((3125LL * TIMER_USEC) / 4LL);
			/* mfm->callback = 10LL * MFM_TIME; */
			timer_update_outstanding();
		} else {
			ui_sb_update_icon(SB_HDD|HDD_BUS_MFM, 0);
		}
	}
    }

    return(ret);
}


static uint8_t
mfm_read(uint16_t port, void *priv)
{
    mfm_t *mfm = (mfm_t *)priv;
    uint8_t ret = 0xff;

    switch (port) {
	case 0x01f0:	/* data */
		ret = mfm_readw(port, mfm) & 0xff;
		break;

	case 0x01f1:	/* error */
		ret = mfm->error;
		break;

	case 0x01f2:	/* sector count */
		ret = mfm->secount;
		break;

	case 0x01f3:	/* sector */
		ret = mfm->sector;
		break;

	case 0x01f4:	/* CYlinder low */
		ret = (uint8_t)(mfm->cylinder&0xff);
		break;

	case 0x01f5:	/* Cylinder high */
		ret = (uint8_t)(mfm->cylinder>>8);
		break;

	case 0x01f6:	/* drive/head */
		ret = (uint8_t)(0xa0 | mfm->head | (mfm->drvsel?0x10:0));
		break;

	case 0x01f7:	/* Status */
		irq_lower(mfm);
		ret = mfm->status;
		break;

	default:
		break;
    }

    mfm_at_log("WD1003 read(%04x) = %02x\n", port, ret);

    return(ret);
}


static void
do_seek(mfm_t *mfm)
{
    drive_t *drive = &mfm->drives[mfm->drvsel];

    mfm_at_log("WD1003(%d) seek(%d) max=%d\n",
	mfm->drvsel,mfm->cylinder,drive->tracks);

    if (mfm->cylinder < drive->tracks)
	drive->curcyl = mfm->cylinder;
      else
	drive->curcyl = drive->tracks-1;
}


static void
do_callback(void *priv)
{
    mfm_t *mfm = (mfm_t *)priv;
    drive_t *drive = &mfm->drives[mfm->drvsel];
    off64_t addr;

    mfm->callback = 0LL;
    if (mfm->reset) {
	mfm_at_log("WD1003(%d) reset\n", mfm->drvsel);

	mfm->status = STAT_READY|STAT_DSC;
	mfm->error = 1;
	mfm->secount = 1;
	mfm->sector = 1;
	mfm->head = 0;
	mfm->cylinder = 0;

	drive->steprate = 0x0f;		/* default steprate */
	drive->cfg_spt = 0;		/* need new parameters */

	mfm->reset = 0;

	ui_sb_update_icon(SB_HDD|HDD_BUS_MFM, 0);

	return;
    }

    switch (mfm->command) {
	case CMD_SEEK:
		mfm_at_log("WD1003(%d) seek, step=%d\n",
			mfm->drvsel, drive->steprate);
		do_seek(mfm);
		mfm->status = STAT_READY|STAT_DSC;
		irq_raise(mfm);
		break;

	case CMD_READ:
		mfm_at_log("WD1003(%d) read(%d,%d,%d)\n",
			mfm->drvsel, mfm->cylinder, mfm->head, mfm->sector);
		do_seek(mfm);
		if (get_sector(mfm, &addr)) {
			mfm->error = ERR_ID_NOT_FOUND;
			mfm->status = STAT_READY|STAT_DSC|STAT_ERR;
			irq_raise(mfm);
			break;
		}

		hdd_image_read(drive->hdd_num, addr, 1, (uint8_t *)mfm->buffer);

		mfm->pos = 0;
		mfm->status = STAT_DRQ|STAT_READY|STAT_DSC;
		irq_raise(mfm);
		ui_sb_update_icon(SB_HDD|HDD_BUS_MFM, 1);
		break;

	case CMD_WRITE:
		mfm_at_log("WD1003(%d) write(%d,%d,%d)\n",
			mfm->drvsel, mfm->cylinder, mfm->head, mfm->sector);
		do_seek(mfm);
		if (get_sector(mfm, &addr)) {
			mfm->error = ERR_ID_NOT_FOUND;
			mfm->status = STAT_READY|STAT_DSC|STAT_ERR;
			irq_raise(mfm);
			break;
		}

		hdd_image_write(drive->hdd_num, addr, 1,(uint8_t *)mfm->buffer);
		irq_raise(mfm);
		mfm->secount = (mfm->secount - 1) & 0xff;

		mfm->status = STAT_READY|STAT_DSC;
		if (mfm->secount) {
			/* More sectors to do.. */
			mfm->status |= STAT_DRQ;
			mfm->pos = 0;
			next_sector(mfm);
			ui_sb_update_icon(SB_HDD|HDD_BUS_MFM, 1);
		} else
			ui_sb_update_icon(SB_HDD|HDD_BUS_MFM, 0);
		break;

	case CMD_VERIFY:
		mfm_at_log("WD1003(%d) verify(%d,%d,%d)\n",
			mfm->drvsel, mfm->cylinder, mfm->head, mfm->sector);
		do_seek(mfm);
		mfm->pos = 0;
		mfm->status = STAT_READY|STAT_DSC;
		irq_raise(mfm);
		ui_sb_update_icon(SB_HDD|HDD_BUS_MFM, 1);
		break;

	case CMD_FORMAT:
		mfm_at_log("WD1003(%d) format(%d,%d)\n",
			mfm->drvsel, mfm->cylinder, mfm->head);
		do_seek(mfm);
		if (get_sector(mfm, &addr)) {
			mfm->error = ERR_ID_NOT_FOUND;
			mfm->status = STAT_READY|STAT_DSC|STAT_ERR;
			irq_raise(mfm);
			break;
		}

		hdd_image_zero(drive->hdd_num, addr, mfm->secount);

		mfm->status = STAT_READY|STAT_DSC;
		irq_raise(mfm);
		ui_sb_update_icon(SB_HDD|HDD_BUS_MFM, 1);
		break;

	case CMD_DIAGNOSE:
		mfm_at_log("WD1003(%d) diag\n", mfm->drvsel);
		drive->steprate = 0x0f;
		mfm->error = 1;
		mfm->status = STAT_READY|STAT_DSC;
		irq_raise(mfm);
		break;

	default:
		mfm_at_log("WD1003(%d) callback on unknown command %02x\n",
					mfm->drvsel, mfm->command);
		mfm->status = STAT_READY|STAT_ERR|STAT_DSC;
		mfm->error = ERR_ABRT;
		irq_raise(mfm);
		break;
    }
}


static void
loadhd(mfm_t *mfm, int c, int d, const wchar_t *fn)
{
    drive_t *drive = &mfm->drives[c];

    if (! hdd_image_load(d)) {
	drive->present = 0;

	return;
    }

    drive->spt = hdd[d].spt;
    drive->hpc = hdd[d].hpc;
    drive->tracks = hdd[d].tracks;
    drive->hdd_num = d;
    drive->present = 1;
}


static void *
mfm_init(const device_t *info)
{
    mfm_t *mfm;
    int c, d;

    mfm_at_log("WD1003: ISA MFM/RLL Fixed Disk Adapter initializing ...\n");
    mfm = malloc(sizeof(mfm_t));
    memset(mfm, 0x00, sizeof(mfm_t));

    c = 0;
    for (d=0; d<HDD_NUM; d++) {
	if ((hdd[d].bus == HDD_BUS_MFM) && (hdd[d].mfm_channel < MFM_NUM)) {
		loadhd(mfm, hdd[d].mfm_channel, d, hdd[d].fn);

		mfm_at_log("WD1003(%d): (%ls) geometry %d/%d/%d\n", c, hdd[d].fn,
			(int)hdd[d].tracks, (int)hdd[d].hpc, (int)hdd[d].spt);

		if (++c >= MFM_NUM) break;
	}
    }

    mfm->status = STAT_READY|STAT_DSC;		/* drive is ready */
    mfm->error = 1;				/* no errors */

    io_sethandler(0x01f0, 1,
		  mfm_read, mfm_readw, NULL, mfm_write, mfm_writew, NULL, mfm);
    io_sethandler(0x01f1, 7,
		  mfm_read, NULL,      NULL, mfm_write, NULL,       NULL, mfm);
    io_sethandler(0x03f6, 1,
		  NULL,     NULL,      NULL, mfm_write, NULL,       NULL, mfm);

    timer_add(do_callback, &mfm->callback, &mfm->callback, mfm);	

    ui_sb_update_icon(SB_HDD|HDD_BUS_MFM, 0);

    return(mfm);
}


static void
mfm_close(void *priv)
{
    mfm_t *mfm = (mfm_t *)priv;
    int d;

    for (d=0; d<2; d++) {
	drive_t *drive = &mfm->drives[d];

	hdd_image_close(drive->hdd_num);		
    }

    free(mfm);

    ui_sb_update_icon(SB_HDD|HDD_BUS_MFM, 0);
}


const device_t mfm_at_wd1003_device = {
    "WD1003 AT MFM/RLL Controller",
    DEVICE_ISA | DEVICE_AT,
    0,
    mfm_init, mfm_close, NULL,
    NULL, NULL, NULL, NULL
};
