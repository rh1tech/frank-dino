/* The host FatFs shim: plain stdio under the firmware's API. */
#include "ff.h"
/* <dirent.h> is not included: its DIR would collide with FatFs's. The
 * three calls this needs are declared by hand against an opaque type. */
typedef struct __host_dir __host_dir;
extern __host_dir *opendir(const char *);
extern int         closedir(__host_dir *);
struct host_dirent_hdr;
extern struct host_dirent_hdr *readdir(__host_dir *);
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Tests run from the repo root; a firmware path like "/disks/a.dsk" is
 * taken as relative to whatever tests/fixtures the test set up. */
static const char *host_path(const TCHAR *p) {
    static char buf[1024];
    snprintf(buf, sizeof buf, "%s", (*p == '/') ? p + 1 : p);
    return buf;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode) {
    const char *m = (mode & FA_CREATE_ALWAYS) ? "w+b"
                  : (mode & (FA_WRITE))       ? "r+b"
                                              : "rb";
    fp->fp = fopen(host_path(path), m);
    if (!fp->fp && (mode & (FA_CREATE_NEW | FA_OPEN_ALWAYS)))
        fp->fp = fopen(host_path(path), "w+b");
    if (!fp->fp) return FR_NO_FILE;
    fseek(fp->fp, 0, SEEK_END);
    fp->sz = (FSIZE_t)ftell(fp->fp);
    fseek(fp->fp, 0, SEEK_SET);
    return FR_OK;
}
FRESULT f_close(FIL *fp) { if (fp->fp) fclose(fp->fp); fp->fp = NULL; return FR_OK; }
FRESULT f_read(FIL *fp, void *b, UINT n, UINT *br) {
    const size_t g = fread(b, 1, n, fp->fp); if (br) *br = (UINT)g; return FR_OK;
}
FRESULT f_write(FIL *fp, const void *b, UINT n, UINT *bw) {
    const size_t g = fwrite(b, 1, n, fp->fp); if (bw) *bw = (UINT)g;
    if ((FSIZE_t)ftell(fp->fp) > fp->sz) fp->sz = (FSIZE_t)ftell(fp->fp);
    return FR_OK;
}
FRESULT f_lseek(FIL *fp, FSIZE_t o) { return fseek(fp->fp, (long)o, SEEK_SET) ? FR_DISK_ERR : FR_OK; }
FRESULT f_sync(FIL *fp)  { fflush(fp->fp); return FR_OK; }
FSIZE_t f_size_fn(FIL *fp) { return fp->sz; }
FSIZE_t f_tell_fn(FIL *fp) { return (FSIZE_t)ftell(fp->fp); }
FRESULT f_stat(const TCHAR *path, FILINFO *fno) {
    struct stat st;
    if (stat(host_path(path), &st)) return FR_NO_FILE;
    if (fno) {
        fno->fsize = (FSIZE_t)st.st_size;
        fno->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : 0;
        snprintf(fno->fname, sizeof fno->fname, "%s", host_path(path));
    }
    return FR_OK;
}
FRESULT f_unlink(const TCHAR *p) { return remove(host_path(p)) ? FR_NO_FILE : FR_OK; }
FRESULT f_mkdir(const TCHAR *p)  { return mkdir(host_path(p), 0777) ? FR_EXIST : FR_OK; }
FRESULT f_mount(FATFS *fs, const TCHAR *p, BYTE o) { (void)fs; (void)p; (void)o; return FR_OK; }
FRESULT f_opendir(DIR *dp, const TCHAR *p) {
    snprintf(dp->path, sizeof dp->path, "%s", host_path(p));
    dp->d = opendir(dp->path); return dp->d ? FR_OK : FR_NO_PATH;
}
FRESULT f_closedir(DIR *dp) { if (dp->d) closedir(dp->d); dp->d = NULL; return FR_OK; }
FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    /* struct dirent, by offset: d_ino(8) d_seekoff(8) d_reclen(2)
     * d_namlen(2) d_type(1) d_name[]  -- the macOS 64-bit layout. */
    const unsigned char *e = (const unsigned char *)(dp->d ? readdir(dp->d) : 0);
    if (!e) { fno->fname[0] = 0; return FR_OK; }
    const char *name = (const char *)(e + 21);
    const unsigned char type = e[20];
    snprintf(fno->fname, sizeof fno->fname, "%s", name);
    fno->fattrib = (type == 4 /* DT_DIR */) ? AM_DIR : 0;
    char full[1200]; snprintf(full, sizeof full, "%s/%s", dp->path, name);
    struct stat st; fno->fsize = stat(full, &st) ? 0 : (FSIZE_t)st.st_size;
    return FR_OK;
}
TCHAR *f_gets(TCHAR *b, int n, FIL *fp) { return fgets(b, n, fp->fp); }
int f_printf(FIL *fp, const TCHAR *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const int r = vfprintf(fp->fp, fmt, ap); va_end(ap); return r;
}
