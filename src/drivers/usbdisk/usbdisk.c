/*
 * frank-dino — an RP2350B acting as the whole machine around a real Z80
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-dino
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "usbdisk.h"

#include <stdio.h>
#include <string.h>

#include <pico/stdlib.h>

#include "ff.h"
#include "diskio.h"

/* The SD card's half of the router, renamed out of the way in sdcard.c. */
DSTATUS sd_disk_initialize(BYTE drv);
DSTATUS sd_disk_status(BYTE drv);
DRESULT sd_disk_read(BYTE drv, BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_write(BYTE drv, const BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_ioctl(BYTE drv, BYTE cmd, void *buff);

#define DRV_SD  0
#define DRV_USB 1

#if USB_KEYBOARD

#include "tusb.h"

static uint8_t  dev_addr;
static bool     present;
static bool     mounted;
static FATFS    usb_fs;
static uint32_t block_count, block_size;

/*
 * The one transfer in flight, and whether it finished.
 *
 * tuh_msc_read10() is asynchronous: it queues the command and returns,
 * and the completion callback fires from inside tuh_task(). Everything
 * that calls into FatFs is synchronous, so the wait below pumps
 * tuh_task() until the callback lands -- which is the only way to bridge
 * the two without a thread.
 */
static volatile bool io_done, io_ok;

bool     usbdisk_present(void) { return present && mounted; }
uint64_t usbdisk_size(void) {
    return present ? (uint64_t)block_count * block_size : 0;
}

static bool io_complete(uint8_t addr, tuh_msc_complete_data_t const *cb) {
    (void)addr;
    io_ok = (cb->csw->status == 0);
    io_done = true;
    return true;
}

/*
 * Wait for the transfer, pumping the host stack.
 *
 * The timeout is what stops a stick pulled mid-read from hanging the
 * machine: the callback for a transfer to a device that is gone never
 * comes, and without this the browser would stop for ever inside a
 * directory listing.
 */
static bool io_wait(const uint32_t ms) {
    const absolute_time_t deadline = make_timeout_time_ms(ms);
    while (!io_done) {
        tuh_task();
        if (!present) return false;
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) return false;
    }
    return io_ok;
}

void tuh_msc_mount_cb(const uint8_t addr) {
    dev_addr = addr;
    block_count = (uint32_t)tuh_msc_get_block_count(addr, 0);
    block_size  = (uint32_t)tuh_msc_get_block_size(addr, 0);
    present = true;
    printf("[usbdisk] %lu blocks of %lu on address %u\n",
           (unsigned long)block_count, (unsigned long)block_size, addr);
}

void tuh_msc_umount_cb(const uint8_t addr) {
    if (addr != dev_addr) return;
    present = false;
    /*
     * Unmount rather than leave FatFs holding a volume whose medium has
     * gone: the next f_open would read a cached sector of a stick that
     * is in somebody's pocket.
     */
    if (mounted) { f_unmount(USBDISK_VOLUME); mounted = false; }
    printf("[usbdisk] gone\n");
}

void usbdisk_task(void) {
    if (present && !mounted) {
        /* 0 = mount lazily... except we want to know now, so force it. */
        mounted = (f_mount(&usb_fs, USBDISK_VOLUME, 1) == FR_OK);
        if (!mounted) printf("[usbdisk] no filesystem this can read\n");
    }
}

/* ── the USB half of the router ──────────────────────────────────────── */

static DRESULT usb_rw(BYTE *buff, LBA_t sector, UINT count, const bool write) {
    if (!present) return RES_NOTRDY;

    /*
     * One block at a time. A stick will take a multi-block command, but
     * the buffer FatFs hands over is not guaranteed to survive a retry
     * and the browser's reads are single blocks anyway -- so this is the
     * simple thing until something needs the speed.
     */
    while (count--) {
        io_done = false;
        io_ok = false;
        const bool queued = write
            ? tuh_msc_write10(dev_addr, 0, buff, (uint32_t)sector, 1, io_complete, 0)
            : tuh_msc_read10(dev_addr, 0, buff, (uint32_t)sector, 1, io_complete, 0);
        if (!queued) return RES_ERROR;
        if (!io_wait(2000)) return RES_ERROR;
        buff += block_size;
        sector++;
    }
    return RES_OK;
}

#else   /* no host stack in this build */

bool     usbdisk_present(void) { return false; }
uint64_t usbdisk_size(void)    { return 0; }
void     usbdisk_task(void)    { }

#endif

/* ── the router ──────────────────────────────────────────────────────── */
/*
 * FatFs's four entry points, dispatching on the drive number.
 *
 * They used to be the SD driver's own, with "if (drv) return" at the top
 * of each -- which is why a second volume had nowhere to go.
 */
DSTATUS disk_initialize(const BYTE drv) {
    if (drv == DRV_SD) return sd_disk_initialize(drv);
#if USB_KEYBOARD
    if (drv == DRV_USB) return present ? 0 : STA_NOINIT;
#endif
    return STA_NOINIT;
}

DSTATUS disk_status(const BYTE drv) {
    if (drv == DRV_SD) return sd_disk_status(drv);
#if USB_KEYBOARD
    if (drv == DRV_USB) return present ? 0 : STA_NOINIT;
#endif
    return STA_NOINIT;
}

DRESULT disk_read(const BYTE drv, BYTE *buff, const LBA_t sector, const UINT count) {
    if (drv == DRV_SD) return sd_disk_read(drv, buff, sector, count);
#if USB_KEYBOARD
    if (drv == DRV_USB) return usb_rw(buff, sector, count, false);
#endif
    return RES_PARERR;
}

DRESULT disk_write(const BYTE drv, const BYTE *buff, const LBA_t sector, const UINT count) {
    if (drv == DRV_SD) return sd_disk_write(drv, buff, sector, count);
#if USB_KEYBOARD
    if (drv == DRV_USB) return usb_rw((BYTE *)buff, sector, count, true);
#endif
    return RES_PARERR;
}

DRESULT disk_ioctl(const BYTE drv, const BYTE cmd, void *buff) {
    if (drv == DRV_SD) return sd_disk_ioctl(drv, cmd, buff);
#if USB_KEYBOARD
    if (drv == DRV_USB) {
        if (!present) return RES_NOTRDY;
        switch (cmd) {
            case CTRL_SYNC:        return RES_OK;
            case GET_SECTOR_COUNT: *(LBA_t *)buff = block_count; return RES_OK;
            case GET_SECTOR_SIZE:  *(WORD *)buff = (WORD)block_size; return RES_OK;
            case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
            default:               return RES_PARERR;
        }
    }
#endif
    return RES_PARERR;
}
