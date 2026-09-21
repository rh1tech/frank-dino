/*
 * A host stand-in for the FatFs subset the firmware uses.
 *
 * This shadows src/drivers/fatfs/ff.h so a test can open a .dsk, a
 * .dck or a card image straight out of the working directory. FatFs
 * itself is vendored and not ours to test; what these tests are for is
 * how our own modules drive it, so the boundary is drawn at the API.
 */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef unsigned int UINT;
typedef uint64_t     FSIZE_t;
typedef uint8_t      BYTE;
typedef uint32_t     DWORD;
typedef uint16_t     WORD;
typedef char         TCHAR;

typedef enum {
    FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
    FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT, FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM, FR_TIMEOUT, FR_LOCKED,
    FR_NOT_ENOUGH_CORE, FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER
} FRESULT;

#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_NEW    0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS   0x10
#define FA_OPEN_APPEND   0x30

#define AM_RDO  0x01
#define AM_HID  0x02
#define AM_SYS  0x04
#define AM_DIR  0x10
#define AM_ARC  0x20

typedef struct { FILE *fp; FSIZE_t sz; } FIL;
typedef struct { void *d; char path[512]; } DIR;
typedef struct { FSIZE_t fsize; WORD fdate, ftime; BYTE fattrib; char fname[256]; } FILINFO;
typedef struct { int dummy; } FATFS;

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_sync(FIL *fp);
FRESULT f_stat(const TCHAR *path, FILINFO *fno);
FRESULT f_unlink(const TCHAR *path);
FRESULT f_mkdir(const TCHAR *path);
FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt);
FRESULT f_opendir(DIR *dp, const TCHAR *path);
FRESULT f_closedir(DIR *dp);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
TCHAR  *f_gets(TCHAR *buff, int len, FIL *fp);
int     f_printf(FIL *fp, const TCHAR *fmt, ...);

FSIZE_t f_size_fn(FIL *fp);
FSIZE_t f_tell_fn(FIL *fp);
#define f_size(fp) f_size_fn(fp)
#define f_tell(fp) f_tell_fn(fp)
