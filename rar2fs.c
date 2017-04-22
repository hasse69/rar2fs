/*
    Copyright (C) 2009 Hans Beckerus (hans.beckerus#AT#gmail.com)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    This program take use of the freeware "Unrar C++ Library" (libunrar)
    by Alexander Roshal and some extensions to it.

    Unrar source may be used in any software to handle RAR archives
    without limitations free of charge, but cannot be used to re-create
    the RAR compression algorithm, which is proprietary. Distribution
    of modified Unrar source in separate form or as a part of other
    software is permitted, provided that it is clearly stated in
    the documentation and source comments that the code may not be used
    to develop a RAR (WinRAR) compatible archiver.
*/
#include <platform.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <fuse.h>
#include <fcntl.h>
#include <getopt.h>
#include <syslog.h>
#include <wchar.h>
#ifdef HAVE_LOCALE_H
# include <locale.h>
#endif
#ifdef HAVE_MMAP
# include <sys/mman.h>
#endif
#include <limits.h>
#include <pthread.h>
#include <ctype.h>
#ifdef HAVE_SCHED_H
# include <sched.h>
#endif
#ifdef HAVE_SYS_XATTR_H
# include <sys/xattr.h>
#endif
#ifdef HAVE_SYS_SYSMACROS_H
# include <sys/sysmacros.h>
#endif
#ifdef HAVE_ICONV
#include <iconv.h>
#endif
#include <assert.h>
#include "version.h"
#include "debug.h"
#include "index.h"
#include "dllwrapper.h"
#include "filecache.h"
#include "dircache.h"
#include "iobuffer.h"
#include "optdb.h"
#include "sighandler.h"
#include "dirlist.h"

#define E_TO_MEM 0
#define E_TO_TMP 1

#define MOUNT_FOLDER  0
#define MOUNT_ARCHIVE 1

#define SYNCDIR_NO_FORCE 0
#define SYNCDIR_FORCE 1

#define RD_IDLE 0
#define RD_WAKEUP 1
#define RD_SYNC_NOREAD 2
#define RD_SYNC_READ 3
#define RD_READ_ASYNC 4

struct volume_handle {
        FILE *fp;
        off_t pos;
};

struct io_context {
        FILE* fp;
        off_t pos;
        struct io_buf *buf;
        pid_t pid;
        unsigned int seq;
        short vno;
        short vno_max;
        struct filecache_entry *entry_p;
        struct volume_handle *volh;
        pthread_t thread;
        pthread_mutex_t rd_req_mutex;
        pthread_cond_t rd_req_cond;
        int rd_req;
        /* debug */
#ifdef DEBUG_READ
        FILE *dbg_fp;
#endif
};

struct io_handle {
        int type;
#define IO_TYPE_NRM 0
#define IO_TYPE_RAR 1
#define IO_TYPE_RAW 2
#define IO_TYPE_ISO 3
#define IO_TYPE_INFO 4
#define IO_TYPE_DIR 5
        union {
                struct io_context *context;     /* type = IO_TYPE_RAR/IO_TYPE_RAW */
                int fd;                         /* type = IO_TYPE_NRM/IO_TYPE_ISO */
                DIR *dp;                        /* type = IO_TYPE_DIR */
                void *buf_p;                    /* type = IO_TYPE_INFO */
                uintptr_t bits;
        } u;
        struct filecache_entry *entry_p;        /* type = IO_TYPE_ISO */
        char *path;                             /* type = all */
};

#define FH_ZERO(fh)            ((fh) = 0)
#define FH_ISSET(fh)           (fh)
#define FH_SETCONTEXT(fh, v)   (FH_TOIO(fh)->u.context = (v))
#define FH_SETFD(fh, v)        (FH_TOIO(fh)->u.fd = (v))
#define FH_SETDP(fh, v)        (FH_TOIO(fh)->u.dp = (v))
#define FH_SETBUF(fh, v)       (FH_TOIO(fh)->u.buf_p = (v))
#define FH_SETIO(fh, v)        ((fh) = (uintptr_t)(v))
#define FH_SETENTRY(fh, v)     (FH_TOIO(fh)->entry_p = (v))
#define FH_SETPATH(fh, v)      (FH_TOIO(fh)->path = (v))
#define FH_SETTYPE(fh, v)      (FH_TOIO(fh)->type = (v))
#define FH_TOCONTEXT(fh)       (FH_TOIO(fh)->u.context)
#define FH_TOFD(fh)            (FH_TOIO(fh)->u.fd)
#define FH_TODP(fh)            (FH_TOIO(fh)->u.dp)
#define FH_TOBUF(fh)           (FH_TOIO(fh)->u.buf_p)
#define FH_TOENTRY(fh)         (FH_TOIO(fh)->entry_p)
#define FH_TOPATH(fh)          (FH_TOIO(fh)->path)
#define FH_TOIO(fh)            ((struct io_handle*)(uintptr_t)(fh))

/* Special DIR pointer for root folder in a file system loop. */
#define FS_LOOP_ROOT_DP        ((DIR *)~0U)

#ifdef HAVE_ICONV
static iconv_t icd;
#endif
static long page_size_ = 0;
static int mount_type;
static struct dir_entry_list arch_list_root;        /* internal list root */
static struct dir_entry_list *arch_list = &arch_list_root;
static pthread_attr_t thread_attr;
static unsigned int rar2_ticks;
static int fs_terminated = 0;
static int fs_loop = 0;
static char *fs_loop_mp_root = NULL;
static char *fs_loop_mp_base = NULL;
static size_t fs_loop_mp_base_len = 0;
static struct stat fs_loop_mp_stat;
static int64_t blkdev_size = -1;
static mode_t umask_ = 0022;

#define P_ALIGN_(a) (((a)+page_size_)&~(page_size_-1))

#ifdef __linux
#define IS_BLKDEV() (blkdev_size >= 0)
#else
#define IS_BLKDEV() (0)
#endif
#define BLKDEV_SIZE() (blkdev_size > 0 ? blkdev_size : 0)

static int extract_rar(char *arch, const char *file, void *arg);
static int get_vformat(const char *s, int t, int *l, int *p);
static int CALLBACK list_callback_noswitch(UINT, LPARAM UserData, LPARAM, LPARAM);

struct eof_cb_arg {
        off_t toff;
        off_t coff;
        size_t size;
        int fd;
        char *arch;
};

struct extract_cb_arg {
        char *arch;
        void *arg;
};

static int extract_index(const char *, const struct filecache_entry *, off_t);
static int preload_index(struct io_buf *, const char *);

#if RARVER_MAJOR > 4
static const char *file_cmd[] = {
        "#info",
        NULL
};
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __dircache_invalidate_for_file(const char *path)
{
        char *safe_path = strdup(path);
        pthread_mutex_lock(&dir_access_mutex);
        dircache_invalidate(dirname(safe_path));
        pthread_mutex_unlock(&dir_access_mutex);
        free(safe_path);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
__attribute__((visibility("hidden")))
void __handle_sigusr1()
{
        printd(3, "Invalidating path cache\n");
        pthread_mutex_lock(&file_access_mutex);
        filecache_invalidate(NULL);
        pthread_mutex_unlock(&file_access_mutex);
        pthread_mutex_lock(&dir_access_mutex);
        dircache_invalidate(NULL);
        pthread_mutex_unlock(&dir_access_mutex);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __wait_thread(struct io_context *op)
{
        pthread_mutex_lock(&op->rd_req_mutex);
        while (op->rd_req) /* sync */
                pthread_cond_wait(&op->rd_req_cond, &op->rd_req_mutex);
        pthread_mutex_unlock(&op->rd_req_mutex);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __wake_thread(struct io_context *op, int req)
{
        pthread_mutex_lock(&op->rd_req_mutex);
        while (op->rd_req) /* sync */
                pthread_cond_wait(&op->rd_req_cond, &op->rd_req_mutex);
        op->rd_req = req;
        pthread_cond_signal(&op->rd_req_cond);
        pthread_mutex_unlock(&op->rd_req_mutex);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int is_rxx_vol(const char *name)
{
        size_t len = strlen(name);
        if (name[len - 4] == '.' &&
                        (name[len - 3] >= 'r' || name[len - 3] >= 'R') &&
                        isdigit(name[len - 2]) && isdigit(name[len - 1])) {
                /* This seems to be a classic .rNN rar volume file.
                 * Let the rar header be the final judge. */
                return 1;
        }
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int is_nnn_vol(const char *name)
{
        size_t len = strlen(name);
        if (name[len - 4] == '.' &&
                        isdigit(name[len - 3]) && isdigit(name[len - 2]) &&
                        isdigit(name[len - 1])) {
                /* This seems to be a .NNN rar volume file.
                 * Let the rar header be the final judge. */
                return 1;
        }
        return 0;
}


#define IS_ISO(s) (!strcasecmp((s)+(strlen(s)-4), ".iso"))
#define IS_AVI(s) (!strcasecmp((s)+(strlen(s)-4), ".avi"))
#define IS_MKV(s) (!strcasecmp((s)+(strlen(s)-4), ".mkv"))
#define IS_RAR(s) (!strcasecmp((s)+(strlen(s)-4), ".rar"))
#define IS_CBR(s) (!OPT_SET(OPT_KEY_NO_EXPAND_CBR) && \
                        !strcasecmp((s)+(strlen(s)-4), ".cbr"))
#define IS_RXX(s) (is_rxx_vol(s))
#define IS_NNN(s) (is_nnn_vol(s))

#define VTYPE(flags) \
        ((flags & MHD_NEWNUMBERING) ? 1 : 0)

/*!
 *****************************************************************************
 *
 ****************************************************************************/
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
static wchar_t *get_password(const char *file, wchar_t *buf, size_t len)
#else
static char *get_password(const char *file, char *buf, size_t len)
#endif
{
        if (file) {
                char *f[2] = {NULL, NULL};
                int l[2] = {0, 0};
                int i;
                int lx;

                /* In case this is a new-style volume we must try to figure
                 * out the file format. */
                (void)get_vformat(file, 1, NULL, &l[0]);
                lx = l[0];
                while (lx && file[lx] != '.') --lx;
                if (lx) {
                        l[0] = lx;
                        f[0] = strdup(file);
                }
                /* In case this is an old-style volume, or even not a volume at
                 * all, simply placing the index behind .rar or .rNN should be
                 * enough to locate the password file. */
                lx = strlen(file);
                if (lx > 4) {
                        l[1] = lx - 4;
                        f[1] = strdup(file);
                }
                for (i = 0; i < 2; i++) {
                        if (f[i]) {
                                char *F = f[i];
                                strcpy(F + l[i], ".pwd");
                                FILE *fp = fopen(F, "r");
                                if (!fp) {
                                        char *tmp1 = strdup(F);
                                        char *tmp2 = strdup(F);
                                        F = malloc(strlen(file) + 8);
                                        sprintf(F, "%s%s%s", dirname(tmp1),
                                                        "/.", basename(tmp2));
                                        free(tmp1);
                                        free(tmp2);
                                        fp = fopen(F, "r");
                                        free(F);
                                }
                                if (fp) {
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
                                        buf = fgetws(buf, len, fp);
                                        if (buf) {
                                                wchar_t *eol = wcspbrk(buf, L"\r\n");
                                                if (eol != NULL)
                                                        *eol = 0;
                                        }
#else
                                        buf = fgets(buf, len, fp);
#endif
                                        fclose(fp);
                                        free(f[0]);
                                        free(f[1]);
                                        return buf;
                                }
                        }
                }
                free(f[0]);
                free(f[1]);
        }
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
size_t wide_to_utf8(const wchar_t *src, char *dst, size_t dst_size)
{
        --dst_size;
        size_t in_size = dst_size;
        ssize_t out_size = dst_size;
        while (*src !=0 && --out_size >= 0) {
                unsigned int c = *(src++);
                if (c < 0x80) {
                        *(dst++) = c;
                        continue;
                }
                if (c < 0x800 && --out_size >= 0) {
                        *(dst++) =(0xc0 | (c >> 6));
                        *(dst++) =(0x80 | (c & 0x3f));
                        continue;
                }
                /* Check for surrogate pair */
                if (c >= 0xd800 && c <= 0xdbff &&
                                *src >= 0xdc00 && *src <= 0xdfff) {
                        c = ((c - 0xd800) << 10) + (*src - 0xdc00) + 0x10000;
                        src++;
                }
                if (c < 0x10000 && (out_size -= 2) >= 0) {
                        *(dst++) = (0xe0 | (c >> 12));
                        *(dst++) = (0x80 | ((c >> 6) & 0x3f));
                        *(dst++) = (0x80 | (c & 0x3f));
                } else if (c < 0x200000 && (out_size -= 3) >= 0) {
                        *(dst++) = (0xf0 | (c >> 18));
                        *(dst++) = (0x80 | ((c >> 12) & 0x3f));
                        *(dst++) = (0x80 | ((c >> 6) & 0x3f));
                        *(dst++) = (0x80 | (c & 0x3f));
                }
        }
        *dst = 0;
        return in_size - out_size;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline size_t
wide_to_char(char *dst, const wchar_t *src, size_t size)
{
        dst[0] = '\0';

#ifdef HAVE_ICONV
        if (icd == (iconv_t)-1)
                goto fallback;

        size_t in_bytes = (wcslen(src) + 1) * sizeof(wchar_t);
        size_t out_bytes = size;
        char *in_buf = (char *)src;
        char *out_buf = dst;
        if (iconv(icd, (ICONV_CONST char **)&in_buf, &in_bytes,
                                        &out_buf, &out_bytes) == (size_t)-1) {
                if (errno == E2BIG) {
                        /* Make sure the buffer is terminated properly but
                         * otherwise keep the truncated result. */
                        dst[size] = '\0';
                        return size;
                } else {
                        *out_buf = '\0';
                        perror("iconv");
                }
        } else {
                return size - out_bytes;
        }

        return -1;

fallback:
#endif
        if (*src) {
#ifdef HAVE_WCSTOMBS
#if 0
                size_t n = wcstombs(NULL, src, 0);
                if (n != (size_t)-1) {
                        if (size >= (n + 1))
                                return wcstombs(dst, src, size);
                }
#endif
#endif
                /* Translation failed! Use fallback to strict UTF-8. */
                return wide_to_utf8(src, dst, size);
        }

        return -1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static struct filecache_entry *path_lookup_miss(const char *path, struct stat *stbuf)
{
        struct stat st;
        char *root;

        printd(3, "MISS    %s\n", path);

        if (fs_loop) {
                  if (!strcmp(path, fs_loop_mp_root)) {
                          memcpy(stbuf, &fs_loop_mp_stat, sizeof(struct stat));
                          return LOCAL_FS_ENTRY;
                  } else {
                          if (!strncmp(path, fs_loop_mp_base, fs_loop_mp_base_len))
                                  return LOOP_FS_ENTRY;
                  }
        }

        ABS_ROOT(root, path);

        /* Check if the missing file can be found on the local fs */
        if (!lstat(root, stbuf ? stbuf : &st)) {
                printd(3, "STAT retrieved for %s\n", root);
                return LOCAL_FS_ENTRY;
        }

        /* Check if the missing file is a fake .iso */
        if (OPT_SET(OPT_KEY_FAKE_ISO) && IS_ISO(root)) {
                struct filecache_entry *e_p;
                int i;
                int obj = OPT_CNT(OPT_KEY_FAKE_ISO)
                        ? OPT_KEY_FAKE_ISO
                        : OPT_KEY_IMG_TYPE;

                /* Try the image file extensions one by one */
                for (i = 0; i < OPT_CNT(obj); i++) {
                        char *tmp = (OPT_STR(obj, i));
                        int l = strlen(tmp ? tmp : "");
                        char *root1 = strdup(root);
                        if (l > 4)
                                root1 = realloc(root1, strlen(root1) + 1 + (l - 4));
                        strcpy(root1 + (strlen(root1) - 4), tmp ? tmp : "");
                        if (lstat(root1, &st)) {
                                free(root1);
                                continue;
                        }
                        e_p = filecache_alloc(path);
                        e_p->file_p = strdup(path);
                        e_p->flags.fake_iso = 1;
                        if (l > 4) {
                                e_p->file_p = realloc(
                                        e_p->file_p,
                                        strlen(path) + 1 + (l - 4));
                        }
                        /* back-patch *real* file name */
                        strncpy(e_p->file_p + (strlen(e_p->file_p) - 4),
                                        tmp ? tmp : "", l);
                        *(e_p->file_p+(strlen(path)-4+l)) = 0;
                        memcpy(&e_p->stat, &st, sizeof(struct stat));
                        if (stbuf)
                                memcpy(stbuf, &st, sizeof(struct stat));
                        free(root1);
                        return e_p;
                }
        }
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static struct filecache_entry *path_lookup(const char *path, struct stat *stbuf)
{
        struct filecache_entry *e_p = filecache_get(path);
        struct filecache_entry *e2_p = e_p;
        if (e_p && !e_p->flags.fake_iso && !e_p->flags.unresolved) {
                if (stbuf)
                        memcpy(stbuf, &e_p->stat, sizeof(struct stat));
                return e_p;
        }
        if (!e_p && mount_type == MOUNT_ARCHIVE && strcmp(path, "/"))
                return NULL;
        /* Do not remember fake .ISO entries between eg. getattr() calls */
        if (e_p && e_p->flags.fake_iso)
                filecache_invalidate(path);
        e_p = path_lookup_miss(path, stbuf);
        if (!e_p) {
                if (e2_p && e2_p->flags.unresolved) {
                        pthread_mutex_lock(&dir_access_mutex);
                        dircache_invalidate(path);
                        pthread_mutex_unlock(&dir_access_mutex);
                        e2_p->flags.unresolved = 0;
                        if (stbuf)
                                memcpy(stbuf, &e2_p->stat, sizeof(struct stat));
                        return e2_p;
                }
        }
        return e_p;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __stop_child(pid_t pid)
{
        pid_t wpid;
        int status = 0;

        killpg(pid, SIGKILL);

        /* Sync */
        do {
                wpid = waitpid(pid, &status, WNOHANG | WUNTRACED);
                if (wpid == -1) {
                        /*
                         * POSIX.1-2001 specifies that if the disposition of
                         * SIGCHLD is set to SIG_IGN or the SA_NOCLDWAIT flag
                         * is set for SIGCHLD (see sigaction(2)), then
                         * children that terminate do not become zombies and
                         * a call to wait() or waitpid() will block until all
                         * children have terminated, and then fail with errno
                         * set to ECHILD.
                         */
                        if (errno != ECHILD)
                                perror("waitpid");
                        return 0;
                }
        } while (!wpid || (!WIFEXITED(status) && !WIFSIGNALED(status)));
        if (WIFEXITED(status))
                return WEXITSTATUS(status);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static FILE *popen_(const struct filecache_entry *entry_p, pid_t *cpid)
{
        int fd = -1;
        int pfd[2] = {-1,};

        pid_t pid;
        if (pipe(pfd) == -1) {
                perror("pipe");
                goto error;
        }

        pid = fork();
        if (pid == 0) {
                int ret;
                setpgid(getpid(), 0);
                close(pfd[0]);  /* Close unused read end */
                ret = extract_rar(entry_p->rar_p,
                                  entry_p->file_p,
                                  (void *)(uintptr_t) pfd[1]);
                close(pfd[1]);
                _exit(ret);
        } else if (pid < 0) {
                /* The fork failed. */
                goto error;
        }

        /* This is the parent process. */
        close(pfd[1]);          /* Close unused write end */
        *cpid = pid;
        return fdopen(pfd[0], "r");

error:
        if (fd >= 0)
                close(fd);
        if (pfd[0] >= 0)
                close(pfd[0]);
        if (pfd[1] >= 0)
                close(pfd[1]);

        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int pclose_(FILE *fp, pid_t pid)
{
        fclose(fp);
        return __stop_child(pid);
}

/* Size of file in first volume number in which it exists */
#define VOL_FIRST_SZ op->entry_p->vsize_first

/*
 * Size of file in the following volume numbers (if situated in more than one).
 * Compared to VOL_FIRST_SZ this is a qualified guess value only and it is
 * not uncommon that it actually becomes equal to VOL_FIRST_SZ.
 *   For reference VOL_NEXT_SZ is basically the end of file data offset in
 * first volume file reduced by the size of applicable headers and meta
 * data. For the last volume file this value is more than likely bogus.
 * This does not matter since the total file size is still reported
 * correctly and anything trying to read it should stop once reaching EOF.
 * The read function will infact verify that this is always the case and
 * throw an error if trying to read beyond EOF. The important thing here
 * is that the volumes files other than the first and last match this value.
 */
#define VOL_NEXT_SZ op->entry_p->vsize_next

/* Size of file data (including headers) in first volume number */
#define VOL_REAL_SZ(x) \
        ((x)?op->entry_p->vsize_real_next:op->entry_p->vsize_real_first)

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static char *get_vname(int t, const char *str, int vol, int len, int pos)
{
        ENTER_("%s   vol=%d, len=%d, pos=%d", str, vol, len, pos);
        char *s = strdup(str);
        if (t) {
                char f[16];
                char f1[16];
                sprintf(f1, "%%0%dd", len);
                sprintf(f, f1, vol);
                strncpy(&s[pos], f, len);
        } else {
                char f[16];
                int lower = s[pos - 1] >= 'r';
                if (vol == 0) {
                        sprintf(f, "%s", (lower ? "ar" : "AR"));
                } else if (vol <= 100) {
                        sprintf(f, "%02d", (vol - 1));
                } else { /* Possible, but unlikely */
                        sprintf(f, "%c%02d", (lower ? 'r' : 'R') + (vol - 1) / 100,
                                                (vol - 1) % 100);
                        --pos;
                        ++len;
                }
                strncpy(&s[pos], f, len);
        }
        return s;
}
/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int get_vformat(const char *s, int t, int *l, int *p)
{
        int len = 0;
        int pos = 0;
        int vol = 0;
        const size_t SLEN = strlen(s);
        if (t) {
                const char *c = s + SLEN - 1;
                while (!isdigit(*c) && c > s)
                        c--;

                const char *num = c;
                while (isdigit(*num) && num > s)
                        num--;

                while (num > s && *num != '.') {
                        if (isdigit(*num)) {
                                char *dot = strchr(s, '.');
                                if (dot != NULL && dot < num)
                                        c = num;
                                break;
                        }
                        num--;
                }

                while (c != s) {
                        if (isdigit(*c)) {
                                --c;
                                ++len;
                        } else {
                                break;
                        }
                }
                if (c != s)
                        vol = strtoul(c + 1, NULL, 10);
                else
                        vol = 0;
                pos =  c - s + 1;
        } else {
                int dot = 0;
                len = SLEN - 1;
                while (dot < 1 && len >= 0) {
                        if (s[len--] == '.')
                                ++dot;
                }
                if (len >= 0) {
                        pos = len + 1;
                        len = SLEN - pos;
                        if (len == 4) {
                                pos += 2;
                                len -= 2;
                                if ((s[pos - 1] == 'r' || s[pos - 1] == 'R') &&
                                    (s[pos    ] == 'a' || s[pos    ] == 'A') &&
                                    (s[pos + 1] == 'r' || s[pos + 1] == 'R')) {
                                        vol = 0;
                                } else {
                                        int lower = s[pos - 1] >= 'r';
                                        errno = 0;
                                        vol = strtoul(&s[pos], NULL, 10) + 1 +
                                                /* Possible, but unlikely */
                                                (100 * (s[pos - 1] - (lower ? 'r' : 'R')));
                                        vol = errno ? 0 : vol;
                                }
                        }
                }
        }

        if (l) *l = len;
        if (p) *p = pos;
        return vol;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __check_rar_header(const char *arch)
{
        HANDLE h;
        RAROpenArchiveDataEx d;
        struct RARHeaderDataEx header;
        static char *last_arch = NULL;

        /* Cache and skip if we are trying to inspect the same file over
         * and over again.
         * Note that this is *NOT* a thread safe operation and the resource
         * needs to be protected. Currently it is implictly protected since
         * this function is only called from RARVolNameToFirstName_BUGGED()
         * which holds the file access lock when ever necessary. This might
         * change though and then this logic needs to be revisited! */
        if (last_arch && !strcmp(arch, last_arch))
                return 0;
        free(last_arch);
        last_arch = strdup(arch);

        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = (char *)arch;   /* Horrible cast! But hey... it is the API! */
        d.OpenMode = RAR_OM_LIST_INCSPLIT;
        d.Callback = list_callback_noswitch;
        d.UserData = (LPARAM)arch;
        h = RAROpenArchiveEx(&d);

        /* Check for fault */
        if (d.OpenResult) {
                if (h)
                        RARCloseArchive(h);
                free(last_arch);
                last_arch = NULL;
                return -1;
        }
        if (RARReadHeaderEx(h, &header))  {
                RARCloseArchive(h);
                free(last_arch);
                last_arch = NULL;
                return -1;
        }
        RARCloseArchive(h);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int RARVolNameToFirstName_BUGGED(char *s, int vtype)
{
        int len;
        int pos;

        RARVolNameToFirstName(s, vtype);
        if (!IS_RAR(s) && !IS_NNN(s))
                return -1;
        if (get_vformat(s, !vtype, &len, &pos) == 1) {
                char *s_copy = strdup(s);
                while (--len >= 0)
                        s_copy[pos+len] = '0';
                if (!access(s_copy, F_OK))
                        strcpy(s, s_copy);
                else if (access(s, F_OK)){
                        free(s_copy);
                        return -1;
                }
                free(s_copy);
                return __check_rar_header(s);
         } else {
                if (!access(s, F_OK))
                        return __check_rar_header(s);
         }
         return -1;
}

#if _POSIX_TIMERS < 1
#define CLOCK_REALTIME 0

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int clock_gettime(int clk_id, struct timespec *ts)
{
        struct timeval now;
        (void)clk_id; /* not used */
        int rv = gettimeofday(&now, NULL);
        if (rv)
                return rv;
        ts->tv_sec  = now.tv_sec;
        ts->tv_nsec = now.tv_usec * 1000;
        return 0;
}

#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void update_atime(const char* path, struct filecache_entry *entry_p,
                         struct timespec *tp_in)
{
        struct filecache_entry *e_p;
        struct timespec tp[2];
        int dir_updated = 0;

        tp[0].tv_sec = tp_in->tv_sec;
        tp[0].tv_nsec = tp_in->tv_nsec;

        entry_p->stat.st_atime = tp[0].tv_sec;
#ifdef HAVE_STRUCT_STAT_ST_ATIM
        entry_p->stat.st_atim.tv_sec = tp[0].tv_sec;
        entry_p->stat.st_atim.tv_nsec = tp[0].tv_nsec;
#endif
        char* tmp1 = strdup(path);
        e_p = filecache_get(dirname(tmp1));
        free(tmp1);
        if (e_p && S_ISDIR(e_p->stat.st_mode)) {
                dir_updated = 1;
                e_p->stat.st_atime = tp[0].tv_sec;
#ifdef HAVE_STRUCT_STAT_ST_ATIM
                e_p->stat.st_atim.tv_sec = tp[0].tv_sec;
                e_p->stat.st_atim.tv_nsec = tp[0].tv_nsec;
#endif
        }
        if (!dir_updated || OPT_SET(OPT_KEY_ATIME_RAR)) {
#if defined( HAVE_UTIMENSAT ) && defined( AT_SYMLINK_NOFOLLOW )
                tp[1].tv_nsec = UTIME_OMIT;
                tmp1 = strdup(entry_p->rar_p);
                RARVolNameToFirstName_BUGGED(tmp1, !entry_p->vtype);
                char *tmp2 = strdup(tmp1);
                int res = utimensat(0, dirname(tmp2), tp, AT_SYMLINK_NOFOLLOW);
                if (!res && OPT_SET(OPT_KEY_ATIME_RAR)) {
                        for (;;) {
                                res = utimensat(0, tmp1, tp, AT_SYMLINK_NOFOLLOW);
                                if (res == -1 && errno == ENOENT)
                                        break;
                                RARNextVolumeName(tmp1, !entry_p->vtype);
                        }
                }
                free(tmp1);
                free(tmp2);
#endif
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void check_atime(const char *path, struct filecache_entry *entry_p)
{
        struct filecache_entry *e_p;
        struct timespec tp;

        if (!OPT_SET(OPT_KEY_ATIME) && !OPT_SET(OPT_KEY_ATIME_RAR))
                goto no_check_atime;
        if (clock_gettime(CLOCK_REALTIME, &tp))
                goto no_check_atime;
        pthread_mutex_lock(&file_access_mutex);
        e_p = filecache_get(path);
        if (e_p) {
                if (e_p->stat.st_atime <= e_p->stat.st_ctime &&
                    e_p->stat.st_atime <= e_p->stat.st_mtime &&
                    (tp.tv_sec - e_p->stat.st_atime) > 86400) { /* 24h */
                        update_atime(path, e_p, &tp);
                        entry_p->stat = e_p->stat;
                }
        }
        pthread_mutex_unlock(&file_access_mutex);

no_check_atime:
        entry_p->flags.check_atime = 0;
        return;
}

/*!
 ****************************************************************************
 *
 ****************************************************************************/
static int lread_raw(char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        size_t n = 0;
        struct io_context *op = FH_TOCONTEXT(fi->fh);
        op->seq++;

        printd(3, "PID %05d calling %s(), seq = %d, offset=%" PRIu64 "\n",
               getpid(), __func__, op->seq, offset);

        size_t chunk;
        int tot = 0;
        int force_seek = 0;

        /*
         * Handle the case when a user tries to read outside file size.
         * This is especially important to handle here since last file in a
         * volume usually is of much less size than the others and conseqently
         * the chunk based calculation will not detect this.
         */
        if ((off_t)(offset + size) >= op->entry_p->stat.st_size) {
                if (offset > op->entry_p->stat.st_size)
                        return 0;       /* EOF */
                size = op->entry_p->stat.st_size - offset;
        }

        if (op->entry_p->flags.check_atime)
                check_atime(FH_TOPATH(fi->fh), op->entry_p);

        while (size) {
                FILE *fp;
                off_t src_off = 0;
                struct volume_handle *vh = NULL;
                if (op->entry_p->flags.multipart) {
                        int vol;
                        /*
                         * RAR5 (and later?) have a one byte volume number in
                         * the Main Archive Header for volume 1-127 and two
                         * bytes for the rest. Check if we need to compensate.
                         */
                        if (op->entry_p->flags.vsize_fixup_needed) {
                                int vol_contrib = 128 - op->entry_p->vno_base + op->entry_p->vno_first - 1;
                                if (offset >= (VOL_FIRST_SZ + (vol_contrib * VOL_NEXT_SZ))) {
                                        off_t offset_left =
                                                offset - (VOL_FIRST_SZ + (vol_contrib * VOL_NEXT_SZ));
                                        vol = 1 + vol_contrib + (offset_left / (VOL_NEXT_SZ - 1));
                                        chunk = (VOL_NEXT_SZ - 1) - (offset_left % (VOL_NEXT_SZ - 1));
                                        goto vol_ready;
                                }
                        }

                        vol = offset < VOL_FIRST_SZ ? 0 :
                                1 + ((offset - VOL_FIRST_SZ) / VOL_NEXT_SZ);
                        chunk = offset < VOL_FIRST_SZ ? VOL_FIRST_SZ - offset :
                                VOL_NEXT_SZ - ((offset - VOL_FIRST_SZ) % (VOL_NEXT_SZ));

vol_ready:
                        /* keep current open file */
                        if (vol != op->vno) {
                                /* close/open */
                                op->vno = vol;
                                if (op->volh) {
                                        vh = &op->volh[vol];
                                        if (vh->fp) {
                                                fp = vh->fp;
                                                src_off = VOL_REAL_SZ(vol) - chunk;
                                                if (src_off != vh->pos)
                                                        force_seek = 1;
                                                goto seek_check;
                                        }
                                }
                                /*
                                 * It is advisable to return 0 (EOF) here
                                 * rather than -errno at failure. Some media
                                 * players tend to react "better" on that and
                                 * terminate playback as expected.
                                 */
                                char *tmp =
                                    get_vname(op->entry_p->vtype,
                                                op->entry_p->rar_p,
                                                op->vno + op->entry_p->vno_base,
                                                op->entry_p->vlen,
                                                op->entry_p->vpos);
                                if (tmp) {
                                        printd(3, "Opening %s\n", tmp);
                                        fp = fopen(tmp, "r");
                                        free(tmp);
                                        if (fp == NULL) {
                                                perror("open");
                                                return 0;       /* EOF */
                                        }
                                        fclose(op->fp);
                                        op->fp = fp;
                                        force_seek = 1;
                                } else {
                                        return 0;               /* EOF */
                                }
                        } else {
                                if (op->volh && op->volh[vol].fp)
                                        fp = op->volh[vol].fp;
                                else
                                        fp = op->fp;
                        }
seek_check:
                        if (force_seek || offset != op->pos) {
                                src_off = VOL_REAL_SZ(vol) - chunk;
                                printd(3, "SEEK src_off = %" PRIu64 ", "
                                                "VOL_REAL_SZ = %" PRIu64 "\n",
                                                src_off, VOL_REAL_SZ(vol));
                                fseeko(fp, src_off, SEEK_SET);
                                force_seek = 0;
                        }
                        printd(3, "size = %zu, chunk = %zu\n", size, chunk);
                        chunk = size < chunk ? size : chunk;
                } else {
                        fp = op->fp;
                        chunk = size;
                        if (!offset || offset != op->pos) {
                                src_off = offset + op->entry_p->offset;
                                printd(3, "SEEK src_off = %" PRIu64 "\n", src_off);
                                fseeko(fp, src_off, SEEK_SET);
                        }
                }
                n = fread(buf, 1, chunk, fp);
                printd(3, "Read %zu bytes from vol=%d, base=%d\n", n, op->vno,
                       op->entry_p->vno_base);
                if (n != chunk)
                        size = n;

                size -= n;
                offset += n;
                buf += n;
                tot += n;
                op->pos = offset;
                if (vh)
                        vh->pos += n;
        }
        return tot;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lread_info(char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        /* Only allow reading from start of file in 'cat'-like fashion */
        if (!offset) {
            struct RARWcb *wcb = FH_TOBUF(fi->fh);
            int c = wide_to_char(buf, wcb->data, size);
            if (c > 0)
                    return c;
        }
        /* Nothing to output (EOF) */
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int sync_thread_read(struct io_context* op)
{
        if (__wake_thread(op, RD_SYNC_READ))
                return -errno;
        if (__wait_thread(op))
                return -errno;
        return 0;
}

static int sync_thread_noread(struct io_context* op)
{
        if (__wake_thread(op, RD_SYNC_NOREAD))
                return -errno;
        if (__wait_thread(op))
                return -errno;
        return 0;
}


/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lread_rar_idx(char *buf, size_t size, off_t offset,
                struct io_context *op)
{
        int res;
        off_t o = op->buf->idx.data_p->head.offset;
        size_t s = op->buf->idx.data_p->head.size;
        off_t off = (offset - o);

        if (off >= (off_t)s)
                return -EIO;

        size = (off + size) > s
                ? size - ((off + size) - s)
                : size;
        printd(3, "Copying %zu bytes from preloaded offset @ %" PRIu64 "\n",
                                                size, offset);
        if (op->buf->idx.mmap) {
                memcpy(buf, op->buf->idx.data_p->bytes + off, size);
                return size;
        }
        res = pread(op->buf->idx.fd, buf, size, off + sizeof(struct idx_head));
        if (res == -1)
                return -errno;
        return res;
}

#ifdef DEBUG_READ
/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void dump_buf(int seq, FILE *fp, char *buf, off_t offset, size_t size)
{
        int i;
        char out[128];
        char *tmp = out;
        size_t size_saved = size;

        memset(out, 0, 128);
        size = size > 64 ? 64 : size;
        if (fp) {
                fprintf(fp, "seq=%d offset: %" PRIu64 "   size: %zu   buf: %p",
                        seq, offset, size_saved, buf);
                for (i = 0; i < size; i++) {
                        if (!i || !(i % 10)) {
                                sprintf(tmp, "\n%016llx : ", offset + i);
                                tmp += 20;
                        }
                        sprintf(tmp, "%02x ", (uint32_t)*(buf+i) & 0xff);
                        tmp += 3;
                        if (i && !(i % 10)) {
                                fprintf(fp, "%s", out);
                                tmp = out;
                        }
                }
                if (i % 10)
                        fprintf(fp, "%s\n", out);

                if (size_saved >= 128) {
                        buf = buf + size_saved - 64;
                        offset = offset + size_saved - 64;
                        tmp = out;

                        fprintf(fp, "\nlast 64 bytes:");
                        for (i = 0; i < 64; i++) {
                                if (!i || !(i % 10)) {
                                        sprintf(tmp, "\n%016llx : ", offset + i);
                                        tmp += 20;
                                }
                                sprintf(tmp, "%02x ", (uint32_t)*(buf+i) & 0xff);
                                tmp += 3;
                                if (i && !(i % 10)) {
                                        fprintf(fp, "%s", out);
                                        tmp = out;
                                }
                        }
                        if (i % 10)
                                fprintf(fp, "%s\n", out);
                }
                fprintf(fp, "\n");
                fflush(fp);
        }
}
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lread_rar(char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        int n = 0;
        struct io_context* op = FH_TOCONTEXT(fi->fh);
#ifdef DEBUG_READ
        char *buf_saved = buf;
        off_t offset_saved = offset;
#endif

        op->seq++;

        printd(3,
               "PID %05d calling %s(), seq = %d, size=%zu, offset=%"
               PRIu64 "/%" PRIu64 "\n",
               getpid(), __func__, op->seq, size, offset, op->pos);

        if ((off_t)(offset + size) >= op->entry_p->stat.st_size) {
                size = offset < op->entry_p->stat.st_size
                        ? op->entry_p->stat.st_size - offset
                        : 0;    /* EOF */
        }
        if (!size)
                goto out;

        if (op->entry_p->flags.check_atime)
                check_atime(FH_TOPATH(fi->fh), op->entry_p);

        /* Check for exception case */
        if (offset != op->pos) {
check_idx:
                if (op->buf->idx.data_p != MAP_FAILED &&
                                offset >= op->buf->idx.data_p->head.offset) {
                        n = lread_rar_idx(buf, size, offset, op);
                        goto out;
                }
                /* Check for backward read */
                if (offset < op->pos) {
                        printd(3, "seq=%d    history access    offset=%" PRIu64
                                                " size=%zu  op->pos=%" PRIu64
                                                "  split=%d\n",
                                                op->seq,offset, size,
                                                op->pos,
                                                (offset + (off_t)size) > op->pos);
                        if ((uint32_t)(op->pos - offset) <= IOB_HIST_SZ) {
                                size_t pos = offset & (IOB_SZ-1);
                                size_t chunk = (off_t)(offset + size) > op->pos
                                        ? (size_t)(op->pos - offset)
                                        : size;
                                size_t tmp = copyFrom(buf, op->buf, chunk, pos);
                                size -= tmp;
                                buf += tmp;
                                offset += tmp;
                                n += tmp;
                        } else {
                                printd(1, "%s: Input/output error   offset=%" PRIu64
                                                        "  pos=%" PRIu64 "\n",
                                                        __func__,
                                                        offset, op->pos);
                                n = -EIO;
                                goto out;
                        }
                /*
                 * Early reads at offsets reaching the last few percent of the
                 * file is most likely a request for index information.
                 */
                } else if ((((offset - op->pos) / (op->entry_p->stat.st_size * 1.0) * 100) > 95.0 &&
                                op->seq < 10)) {
                        printd(3, "seq=%d    long jump hack1    offset=%" PRIu64 ","
                                                " size=%zu, buf->offset=%" PRIu64 "\n",
                                                op->seq, offset, size,
                                                op->buf->offset);
                        op->seq--;      /* pretend it never happened */

                        /*
                         * If enabled, attempt to extract the index information
                         * based on the offset. If that fails fall-back to best
                         * effort. That is, return all zeros according to size.
                         * In the latter case also force direct I/O since
                         * otherwise the fake data might propagate incorrectly
                         * to sub-sequent reads.
                         */
                        struct filecache_entry *e_p; /* "real" cache entry */
                        if (op->entry_p->flags.save_eof) {
                                pthread_mutex_lock(&file_access_mutex);
                                e_p = filecache_get(FH_TOPATH(fi->fh));
                                if (e_p)
                                        e_p->flags.save_eof = 0;
                                pthread_mutex_unlock(&file_access_mutex);
                                op->entry_p->flags.save_eof = 0;
                                if (!extract_index(FH_TOPATH(fi->fh),
                                                   op->entry_p,
                                                   offset)) {
                                        if (!preload_index(op->buf,
                                                           FH_TOPATH(fi->fh))) {
                                                op->seq++;
                                                goto check_idx;
                                        }
                                }
                        }
                        pthread_mutex_lock(&file_access_mutex);
                        e_p = filecache_get(FH_TOPATH(fi->fh));
                        if (e_p)
                                e_p->flags.direct_io = 1;
                        pthread_mutex_unlock(&file_access_mutex);
                        op->entry_p->flags.direct_io = 1;
                        memset(buf, 0, size);
                        n += size;
                        goto out;
                }
        }

        /*
         * Check if we need to wait for data to arrive.
         * This should not be happening frequently. If it does it is an
         * indication that the I/O buffer is set too small.
         */
        if ((off_t)(offset + size) > op->buf->offset) {
                if (sync_thread_read(op))
                        return -EIO;
                /* If there is still no data assume something went wrong.
                 * Also assume that, if the file is encrypted, the reason
                 * for the error is a missing or invalid password!
                 */
                if (!op->buf->offset)
                        return op->entry_p->flags.encrypted ? -EPERM : -EIO;
        }
        if ((off_t)(offset + size) > op->buf->offset) {
                if (offset >= op->buf->offset) {
                        /*
                         * This is another hack! At this point an early read
                         * far beyond the current stream position is most
                         * likely bogus. We can not blindly take the jump here
                         * since it would render the stream completely useless
                         * for continued playback. If the jump is too far off,
                         * again fall-back to best effort. Also making sure
                         * direct I/O is forced from now on to not cause any
                         * fake data to propagate in sub-sequent reads.
                         * This case is very likely for multi-part AVI 2.0.
                         */
                        if (op->seq < 25 && ((offset + size) - op->buf->offset)
                                        > (IOB_SZ - IOB_HIST_SZ)) {
                                struct filecache_entry *e_p; /* "real" cache entry */
                                printd(3, "seq=%d    long jump hack2    offset=%" PRIu64 ","
                                                " size=%zu, buf->offset=%" PRIu64 "\n",
                                                op->seq, offset, size,
                                                op->buf->offset);
                                op->seq--;      /* pretend it never happened */
                                pthread_mutex_lock(&file_access_mutex);
                                e_p = filecache_get(FH_TOPATH(fi->fh));
                                if (e_p)
                                        e_p->flags.direct_io = 1;
                                pthread_mutex_unlock(&file_access_mutex);
                                op->entry_p->flags.direct_io = 1;
                                memset(buf, 0, size);
                                n += size;
                                goto out;
                        }
                }

                /* Take control of reader thread */
                if (sync_thread_noread(op))
                        return -EIO;
                if (!feof(op->fp) && offset > op->buf->offset) {
                        /* consume buffer */
                        op->pos += op->buf->used;
                        op->buf->ri = op->buf->wi;
                        op->buf->used = 0;
                        (void)readTo(op->buf, op->fp,
                                        IOB_SAVE_HIST);
                        sched_yield();
                }

                if (!feof(op->fp)) {
                        op->buf->ri = offset & (IOB_SZ - 1);
                        op->buf->used -= (offset - op->pos);
                        op->pos = offset;

                        /* Pull in rest of data if needed */
                        if ((size_t)(op->buf->offset - offset) < size)
                                (void)readTo(op->buf, op->fp,
                                                IOB_SAVE_HIST);
                }
        }

        if (size) {
                int off = offset - op->pos;
                n += readFrom(buf, op->buf, size, off);
                op->pos += (off + size);
                if (__wake_thread(op, RD_READ_ASYNC))
                        return -EIO;
        }

out:

#ifdef DEBUG_READ
        if (n > 0)
                dump_buf(op->seq, op->dbg_fp, buf_saved, offset_saved, n);
#endif

        printd(3, "%s: RETURN %d\n", __func__, n);
        return n;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lflush(struct fuse_file_info *fi)
{
        ENTER_();

        (void)fi;               /* touch */

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lrelease(struct fuse_file_info *fi)
{
        ENTER_();

        if (FH_TOIO(fi->fh)->type == IO_TYPE_INFO)
                free(FH_TOBUF(fi->fh));
        else if (FH_TOFD(fi->fh))
                close(FH_TOFD(fi->fh));
        if (FH_TOIO(fi->fh)->type == IO_TYPE_ISO) {
                if (FH_TOENTRY(fi->fh))
                        filecache_freeclone(FH_TOENTRY(fi->fh));
        }
        printd(3, "(%05d) %s [0x%-16" PRIx64 "]\n", getpid(), "FREE", fi->fh);
        free(FH_TOIO(fi->fh));
        FH_ZERO(fi->fh);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lread(char *buffer, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        int res;

        ENTER_("%d   size = %zu, offset = %" PRIu64, FH_TOFD(fi->fh),
                                        size, offset);

        res = pread(FH_TOFD(fi->fh), buffer, size, offset);
        if (res == -1)
                return -errno;
        return res;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lopen(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);
        int fd = open(path, fi->flags);
        if (fd == -1)
                return -errno;
        struct io_handle *io = malloc(sizeof(struct io_handle));
        if (!io) {
                close(fd);
                return -EIO;
        }
        FH_SETIO(fi->fh, io);
        FH_SETTYPE(fi->fh, IO_TYPE_NRM);
        FH_SETENTRY(fi->fh, NULL);
        FH_SETFD(fi->fh, fd);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/

#if !defined ( DEBUG_ ) || DEBUG_ < 5
#define dump_stat(s)
#else
#define DUMP_STATO_(m) \
        fprintf(stderr, "%10s = %o (octal)\n", #m , (unsigned int)stbuf->m)
#define DUMP_STAT4_(m) \
        fprintf(stderr, "%10s = %u\n", #m , (unsigned int)stbuf->m)
#define DUMP_STAT8_(m) \
        fprintf(stderr, "%10s = %" PRIu64 "\n", #m , (uint64_t)stbuf->m)

static void dump_stat(struct stat *stbuf)
{
        fprintf(stderr, "struct stat {\n");
        DUMP_STAT4_(st_dev);
        DUMP_STATO_(st_mode);
        DUMP_STAT4_(st_nlink);
        if (sizeof(stbuf->st_ino) > 4)
                DUMP_STAT8_(st_ino);
        else
                DUMP_STAT4_(st_ino);
        DUMP_STAT4_(st_uid);
        DUMP_STAT4_(st_gid);
        DUMP_STAT4_(st_rdev);
        if (sizeof(stbuf->st_size) > 4)
                DUMP_STAT8_(st_size);
        else
                DUMP_STAT4_(st_size);
#ifdef HAVE_STRUCT_STAT_ST_BLOCKS
        DUMP_STAT4_(st_blocks);
#endif
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
        DUMP_STAT4_(st_blksize);
#endif
#ifdef HAVE_STRUCT_STAT_ST_GEN
        DUMP_STAT4_(st_gen);
#endif
        fprintf(stderr, "}\n");
}

#undef DUMP_STAT4_
#undef DUMP_STAT8_
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int collect_files(const char *arch, struct dir_entry_list *list)
{
        int files = 0;
        HANDLE h;
        RAROpenArchiveDataEx d;

        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = (char *)arch;   /* Horrible cast! But hey... it is the API! */
        d.OpenMode = RAR_OM_LIST_INCSPLIT;
        d.Callback = list_callback_noswitch;
        d.UserData = (LPARAM)arch;
        h = RAROpenArchiveEx(&d);

        /* Check for fault */
        if (d.OpenResult) {
                if (h)
                        RARCloseArchive(h);
                if (d.OpenResult == ERAR_MISSING_PASSWORD)
			files = -EPERM;
                goto out;
        }

        if (d.Flags & MHD_VOLUME) {
                char *arch_ = strdup(arch);
                int format = (d.Flags & MHD_NEWNUMBERING) ? 0 : 1;
                RARVolNameToFirstName_BUGGED(arch_, format);
                while (1) {
                        if (access(arch_, F_OK))
                               break;
                        list = dir_entry_add(list, arch_, NULL, DIR_E_NRM);
                        ++files;
                        RARNextVolumeName(arch_, format);
                }
                free(arch_);
        } else {
                (void)dir_entry_add(list, arch, NULL, DIR_E_NRM);
                files = 1;
        }

out:
        return files;
}

#define IS_UNIX_MODE_(l) \
        ((l)->UnpVer >= 50 \
                ? (l)->HostOS == HOST_UNIX \
                : (l)->HostOS == HOST_UNIX || (l)->HostOS == HOST_BEOS)
#define IS_RAR_DIR(l) \
        ((l)->UnpVer >= 20 \
                ? (((l)->Flags&LHD_DIRECTORY)==LHD_DIRECTORY) \
                : (IS_UNIX_MODE_(l) \
                        ? (l)->FileAttr & S_IFDIR \
                        : (l)->FileAttr & 0x10))
#define GET_RAR_MODE(l) \
                (IS_UNIX_MODE_(l) \
                        ? (l)->FileAttr \
                        : IS_RAR_DIR(l) \
                                ? (S_IFDIR|(0777&~umask_)) \
                                : (S_IFREG|(0666&~umask_)))
#define GET_RAR_SZ(l) \
        (IS_RAR_DIR(l) ? 4096 : (((l)->UnpSizeHigh * 0x100000000ULL) | \
                (l)->UnpSize))
#define GET_RAR_PACK_SZ(l) \
        (IS_RAR_DIR(l) ? 4096 : (((l)->PackSizeHigh * 0x100000000ULL) | \
                (l)->PackSize))

/*!
 ****************************************************************************
 *
 ****************************************************************************/
static int CALLBACK index_callback(UINT msg, LPARAM UserData,
                LPARAM P1, LPARAM P2)
{
        struct eof_cb_arg *eofd = (struct eof_cb_arg *)UserData;

        if (msg == UCM_PROCESSDATA) {
                /*
                 * We do not need to handle the case that not all data is
                 * written after return from write() since the pipe is not
                 * opened using the O_NONBLOCK flag.
                 */
                if (eofd->coff != eofd->toff) {
                        eofd->coff += P2;
                        if (eofd->coff > eofd->toff) {
                                off_t delta = eofd->coff - eofd->toff;
                                if (delta < P2) {
                                        eofd->coff -= delta;
                                        P1 = (LPARAM)((void*)P1 + (P2 - delta));
                                        P2 = delta;
                                }
                        }
                }
                if (eofd->coff == eofd->toff) {
                        eofd->size += P2;
                        NO_UNUSED_RESULT write(eofd->fd, (char *)P1, P2);
                        fdatasync(eofd->fd);      /* XXX needed!? */
                        eofd->toff += P2;
                        eofd->coff = eofd->toff;
                }

        }
        if (msg == UCM_CHANGEVOLUME)
                return access((char *)P1, F_OK);
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
        if (msg == UCM_NEEDPASSWORDW) {
                if (!get_password(eofd->arch, (wchar_t *)P1, P2))
                        return -1;
        }
#else
        if (msg == UCM_NEEDPASSWORD) {
                if (!get_password(eofd->arch, (char *)P1, P2))
                        return -1;
        }
#endif

        return 1;
}

/*!
 ****************************************************************************
 *
 ****************************************************************************/
static int extract_index(const char *path, const struct filecache_entry *entry_p,
                         off_t offset)
{
        int e = ERAR_BAD_DATA;
        struct RAROpenArchiveDataEx d;
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = entry_p->rar_p;
        d.OpenMode = RAR_OM_EXTRACT;

        struct RARHeaderDataEx header;
        HANDLE hdl = 0;
        struct idx_head head = {R2I_MAGIC, 0, 0, 0, 0};
        char *r2i;

        struct eof_cb_arg eofd;
        eofd.toff = offset;
        eofd.coff = 0;
        eofd.size = 0;
        eofd.arch = d.ArcName;

        d.Callback = index_callback;
        d.UserData = (LPARAM)&eofd;

        ABS_ROOT(r2i, path);
        strcpy(&r2i[strlen(r2i) - 3], "r2i");

        eofd.fd = open(r2i, O_WRONLY|O_CREAT|O_EXCL,
                        S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
        if (eofd.fd == -1)
                goto index_error;
        lseek(eofd.fd, sizeof(struct idx_head), SEEK_SET);

        hdl = RAROpenArchiveEx(&d);
        if (!hdl || d.OpenResult)
                goto index_error;

        header.CmtBufSize = 0;
        while (1) {
                if (RARReadHeaderEx(hdl, &header))
                        break;
                /* We won't extract subdirs */
                if (IS_RAR_DIR(&header) ||
                        strcmp(header.FileName, entry_p->file_p)) {
                                if (RARProcessFile(hdl, RAR_SKIP, NULL, NULL))
                                        break;
                } else {
                        e = RARProcessFile(hdl, RAR_TEST, NULL, NULL);
                        if (!e) {
                                head.offset = offset;
                                head.size = eofd.size;
                                lseek(eofd.fd, (off_t)0, SEEK_SET);
                                NO_UNUSED_RESULT write(eofd.fd, (void*)&head,
                                                sizeof(struct idx_head));
                        }
                        break;
                }
        }

index_error:
        if (eofd.fd != -1)
                close(eofd.fd);
        if (hdl)
                RARCloseArchive(hdl);
        return e ? -1 : 0;
}

/*!
 ****************************************************************************
 *
 ****************************************************************************/
static int CALLBACK extract_callback(UINT msg, LPARAM UserData,
                LPARAM P1, LPARAM P2)
{
        struct extract_cb_arg *cb_arg = (struct extract_cb_arg *)(UserData);
        if (msg == UCM_PROCESSDATA) {
                /*
                 * We do not need to handle the case that not all data is
                 * written after return from write() since the pipe is not
                 * opened using the O_NONBLOCK flag.
                 */
                int fd = cb_arg->arg ? (LPARAM)cb_arg->arg : STDOUT_FILENO;
                if (write(fd, (void *)P1, P2) == -1) {
                        /*
                         * Do not treat EPIPE as an error. It is the normal
                         * case when the process is terminted, ie. the pipe is
                         * closed since SIGPIPE is not handled.
                         */
                        if (errno != EPIPE)
                                perror("extract_callback: write");
                        return -1;
                }
        }
        if (msg == UCM_CHANGEVOLUME)
                return access((char *)P1, F_OK);
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
        if (msg == UCM_NEEDPASSWORDW) {
                if (!get_password(cb_arg->arch, (wchar_t *)P1, P2))
                        return -1;
        }
#else
        if (msg == UCM_NEEDPASSWORD) {
                if (!get_password(cb_arg->arch, (char *)P1, P2))
                        return -1;
        }
#endif

        return 1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int extract_rar(char *arch, const char *file, void *arg)
{
        int ret = 0;
        struct RAROpenArchiveDataEx d;
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = arch;
        d.OpenMode = RAR_OM_EXTRACT;

        struct extract_cb_arg cb_arg;
        cb_arg.arch = arch;
        cb_arg.arg = arg;

        d.Callback = extract_callback;
        d.UserData = (LPARAM)&cb_arg;
        struct RARHeaderDataEx header;
        HANDLE hdl = RAROpenArchiveEx(&d);
        if (d.OpenResult)
                goto extract_error;

        header.CmtBufSize = 0;
        while (1) {
                if (RARReadHeaderEx(hdl, &header))
                        break;
                /* We won't extract subdirs */
                if (IS_RAR_DIR(&header) || strcmp(header.FileName, file)) {
                        if (RARProcessFile(hdl, RAR_SKIP, NULL, NULL))
                                break;
                } else {
                        ret = RARProcessFile(hdl, RAR_TEST, NULL, NULL);
                        break;
                }
        }

extract_error:

        if (hdl) 
                RARCloseArchive(hdl);

        return ret;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void set_rarstats(struct filecache_entry *entry_p,
                RARArchiveListEx *alist_p,
                int force_dir)
{
        if (!force_dir) {
                mode_t mode = GET_RAR_MODE(&alist_p->hdr);
                entry_p->stat.st_size = GET_RAR_SZ(&alist_p->hdr);
                if (!S_ISDIR(mode) && !S_ISLNK(mode)) {
                        /* Force file to be treated as a 'regular file' */
                        mode = (mode & ~S_IFMT) | S_IFREG;
                }
                if (S_ISLNK(mode)) {
                        if (alist_p->LinkTargetFlags & LINK_T_UNICODE) {
                                char *tmp = malloc(sizeof(alist_p->LinkTarget));
                                if (tmp) {
                                        size_t len = wide_to_char(tmp,
                                                alist_p->LinkTargetW,
                                                sizeof(alist_p->LinkTarget));
                                        if ((int)len != -1) {
                                                entry_p->link_target_p = strdup(tmp);
                                                entry_p->stat.st_size = len;
                                        }
                                        free(tmp);
                                }
                        } else {
                                entry_p->link_target_p =
                                        strdup(alist_p->LinkTarget);
                        }
                }
                entry_p->stat.st_mode = mode;
#ifndef HAVE_SETXATTR
                entry_p->stat.st_nlink =
                        S_ISDIR(mode) ? 2 : alist_p->hdr.Method - (FHD_STORING - 1);
#else
                entry_p->stat.st_nlink =
                        S_ISDIR(mode) ? 2 : 1;
#endif
        } else {
                entry_p->stat.st_mode = (S_IFDIR | (0777 & ~umask_));
                entry_p->stat.st_nlink = 2;
                entry_p->stat.st_size = 4096;
        }
        entry_p->stat.st_uid = getuid();
        entry_p->stat.st_gid = getgid();
        entry_p->stat.st_ino = 0;

#ifdef HAVE_STRUCT_STAT_ST_BLOCKS
        /*
         * This is far from perfect but does the job pretty well!
         * If there is some obvious way to calculate the number of blocks
         * used by a file, please tell me! Most Linux systems seems to
         * apply some sort of multiple of 8 blocks (4K bytes) scheme?
         */
        entry_p->stat.st_blocks =
            (((entry_p->stat.st_size + (8 * 512)) & ~((8 * 512) - 1)) / 512);
#endif
        struct tm t;
        memset(&t, 0, sizeof(struct tm));

        union dos_time_t {
                __extension__ struct {
#ifndef WORDS_BIGENDIAN
                        unsigned int second:5;
                        unsigned int minute:6;
                        unsigned int hour:5;
                        unsigned int day:5;
                        unsigned int month:4;
                        unsigned int year:7;
#else
                        unsigned int year:7;
                        unsigned int month:4;
                        unsigned int day:5;
                        unsigned int hour:5;
                        unsigned int minute:6;
                        unsigned int second:5;
#endif
                };

                /*
                 * Avoid type-punned pointer warning when strict aliasing is used
                 * with some versions of gcc.
                 */
                unsigned int as_uint_;
        };

        /* Using DOS time format by default for backward compatibility. */
        union dos_time_t *dos_time = (union dos_time_t *)&alist_p->hdr.FileTime;

        t.tm_sec = dos_time->second * 2;
        t.tm_min = dos_time->minute;
        t.tm_hour = dos_time->hour;
        t.tm_mday = dos_time->day;
        t.tm_mon = dos_time->month - 1;
        t.tm_year = (1980 + dos_time->year) - 1900;
        t.tm_isdst=-1;
        entry_p->stat.st_atime = mktime(&t);
        entry_p->stat.st_mtime = entry_p->stat.st_atime;
        entry_p->stat.st_ctime = entry_p->stat.st_atime;

        /* Using internally stored 100 ns precision time when available. */
#ifdef HAVE_STRUCT_STAT_ST_MTIM
        if (alist_p->RawTime.mtime) {
                entry_p->stat.st_mtim.tv_sec  =
                        (alist_p->RawTime.mtime / 10000000);
                entry_p->stat.st_mtim.tv_nsec =
                        (alist_p->RawTime.mtime % 10000000) * 100;
        }
#endif
#ifdef HAVE_STRUCT_STAT_ST_CTIM
        if (alist_p->RawTime.ctime) {
                entry_p->stat.st_ctim.tv_sec  =
                        (alist_p->RawTime.ctime / 10000000);
                entry_p->stat.st_ctim.tv_nsec =
                        (alist_p->RawTime.ctime % 10000000) * 100;
        }
#endif
#ifdef HAVE_STRUCT_STAT_ST_ATIM
        if (alist_p->RawTime.atime) {
                entry_p->stat.st_atim.tv_sec  =
                        (alist_p->RawTime.atime / 10000000);
                entry_p->stat.st_atim.tv_nsec =
                        (alist_p->RawTime.atime % 10000000) * 100;
        }
#endif
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
#define DOS_TO_UNIX_PATH(p) \
        do {\
                char *s = (p); \
                while(*s++) \
                        if (*s == '\\') *s = '/'; \
        } while(0)
#define CHRCMP(s, c) (!(s[0] == (c) && s[1] == '\0'))

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static struct filecache_entry *lookup_filecopy(const char *path,
                RARArchiveListEx *next,
                const char *rar_root, int display)

{
        struct filecache_entry *e_p = NULL;
        char *tmp = malloc(sizeof(next->LinkTarget));
        if (tmp) {
                if (wide_to_char(tmp, next->LinkTargetW,
                                        sizeof(next->LinkTarget)) != (size_t)-1) {
                        DOS_TO_UNIX_PATH(tmp);
                        char *mp2;
                        if (!display) {
                                ABS_MP(mp2, (*rar_root ? rar_root : "/"), tmp);
                        } else {
                                char *rar_dir = strdup(tmp);
                                ABS_MP(mp2, path, basename(rar_dir));
                                free(rar_dir);
                        }
                        e_p = filecache_get(mp2);
                }
                free(tmp);
        }
        return e_p;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int CALLBACK list_callback_noswitch(UINT msg,LPARAM UserData,LPARAM P1,LPARAM P2)
{
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
        if (msg == UCM_CHANGEVOLUME || msg == UCM_CHANGEVOLUMEW)
                return -1; /* Do not allow volume switching */
        if (msg == UCM_NEEDPASSWORDW) {
                if (!get_password((char *)UserData, (wchar_t *)P1, P2))
                        return -1;
        }
#else
        if (msg == UCM_CHANGEVOLUME)
                return -1; /* Do not allow volume switching */
        if (msg == UCM_NEEDPASSWORD) {
                if (!get_password((char *)UserData, (char *)P1, P2))
                        return -1;
        }
#endif

        return 1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int CALLBACK list_callback(UINT msg,LPARAM UserData,LPARAM P1,LPARAM P2)
{
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
        if (msg == UCM_NEEDPASSWORDW) {
                if (!get_password((char *)UserData, (wchar_t *)P1, P2))
                        return -1;
        }
#else
        if (msg == UCM_NEEDPASSWORD) {
                if (!get_password((char *)UserData, (char *)P1, P2))
                        return -1;
        }
#endif

        return 1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void __add_filler(const char *path, struct dir_entry_list **buffer,
                const char *file)
{
        size_t path_len;
        size_t file_len;
        char *match;

        if (!buffer)
                return;

        path_len = strlen(path);
        file_len = strlen(file);
        if (path_len > 1) {
        match = strstr(file, path);
                if (!match || path_len == file_len || file[path_len] != '/')
                        return;
                ++path_len;
        }

        /* Cut input file path on current lookup level */
        char *file_dup = strdup(file);
        char *s = file_dup + path_len;
        s = strchr(s, '/');
        if (s)
                *s = '\0';

        struct filecache_entry *entry_p = filecache_get(file_dup);
        if (entry_p != NULL) {
                char *safe_path = strdup(file_dup);
                *buffer = dir_entry_add(*buffer, basename(file_dup),
                                                &entry_p->stat,
                                                DIR_E_RAR);
                free(safe_path);
        }
        free(file_dup);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int listrar(const char *path, struct dir_entry_list **buffer,
                const char *arch, int *final)
{
        ENTER_("%s   arch=%s", path, arch);

        pthread_mutex_lock(&file_access_mutex);
        RAROpenArchiveDataEx d;
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = (char *)arch;   /* Horrible cast! But hey... it is the API! */
        d.OpenMode = RAR_OM_LIST_INCSPLIT;
        d.Callback = list_callback_noswitch;
        d.UserData = (LPARAM)arch;
        HANDLE hdl = RAROpenArchiveEx(&d);

        /* Check for fault */
        if (d.OpenResult) {
                pthread_mutex_unlock(&file_access_mutex);
                if (hdl)
                        RARCloseArchive(hdl);
                return d.OpenResult;
        }

        if (d.Flags & MHD_PASSWORD) {
                RARCloseArchive(hdl);
                d.Callback = list_callback;
                hdl = RAROpenArchiveEx(&d);
                if (final)
                        *final = 1;
        }

        int n_files;
        int dll_result;
        RARArchiveListEx L;
        RARArchiveListEx *next = &L;
        if (!(n_files = RARListArchiveEx(hdl, next, &dll_result))) {
                RARCloseArchive(hdl);
                pthread_mutex_unlock(&file_access_mutex);
                if (dll_result == ERAR_EOPEN || dll_result == ERAR_END_ARCHIVE)
                        return 0;
                return 1;
        }

        char *tmp1 = strdup(arch);
        char *rar_root = strdup(dirname(tmp1));
        free(tmp1);
        tmp1 = rar_root;
        rar_root += strlen(OPT_STR2(OPT_KEY_SRC, 0));
        size_t rar_root_len = strlen(rar_root);
        int is_root_path = (!strcmp(rar_root, path) || !CHRCMP(path, '/'));

        while (next) {
                char *mp;
                int display = 0;

                DOS_TO_UNIX_PATH(next->hdr.FileName);

                /* Skip compressed image files */
                if (!OPT_SET(OPT_KEY_SHOW_COMP_IMG) &&
                                next->hdr.Method != FHD_STORING &&
                                IS_IMG(next->hdr.FileName)) {
                        next = next->next;
                        --n_files;
                        continue;
                }

                if (is_root_path) {
                        /*
                         * Handle the case when the parent folders do not have
                         * their own entry in the file header or is located in
                         * the end. The entries needs to be faked by adding it
                         * to the cache. If the parent folder is discovered
                         * later in the header the faked entry will be
                         * invalidated and replaced with the real file stats.
                         */
                        char *safe_path = strdup(next->hdr.FileName);
                        char *first_arch = strdup(arch);
                        /*
                         * Make sure parent folders are always searched
                         * from the first volume file since sub-folders
                         * might actually be placed elsewhere.
                         */
                        if ((d.Flags & MHD_VOLUME) &&
                                RARVolNameToFirstName_BUGGED(first_arch,
                                                 !VTYPE(d.Flags))) {
                                free(first_arch);
                                first_arch = NULL;
                        }
                        for (;first_arch;) {
                                char *mp;

                                if (!CHRCMP(dirname(safe_path), '.'))
                                        break;
                                ABS_MP(mp, path, safe_path);

                                struct filecache_entry *entry_p = filecache_get(mp);
                                if (entry_p == NULL) {
                                        printd(3, "Adding %s to cache\n", mp);
                                        entry_p = filecache_alloc(mp);
                                        entry_p->rar_p = strdup(first_arch);
                                        entry_p->file_p = strdup(safe_path);
                                        entry_p->flags.force_dir = 1;
                                        entry_p->flags.unresolved = 1;

                                        set_rarstats(entry_p, next, 1);

                                        /* Check if part of a volume */
                                        if (d.Flags & MHD_VOLUME) {
                                                entry_p->flags.multipart = 1;
                                                entry_p->vtype = VTYPE(d.Flags);
                                        } else {
                                                entry_p->flags.multipart = 0;
                                        }
                                }
                        }
                        free(safe_path);
                        free(first_arch);
                        ABS_MP(mp, (*rar_root ? rar_root : "/"),
                                        next->hdr.FileName);
                } else {
                        char *safe_path = strdup(next->hdr.FileName);
                        if (!strcmp(path + rar_root_len + 1,
                                        dirname(safe_path))) {
                                char *rar_dir = strdup(next->hdr.FileName);
                                ABS_MP(mp, path, basename(rar_dir));
                                free(rar_dir);
                                display = 1;
                        } else {
                                ABS_MP(mp, (*rar_root ? rar_root : "/"),
                                                next->hdr.FileName);
                        }
                        free(safe_path);
                }

                if (!IS_RAR_DIR(&next->hdr) && OPT_SET(OPT_KEY_FAKE_ISO)) {
                        int l = OPT_CNT(OPT_KEY_FAKE_ISO)
                                        ? optdb_find(OPT_KEY_FAKE_ISO, mp)
                                        : optdb_find(OPT_KEY_IMG_TYPE, mp);
                        if (l)
                                strcpy(mp + (strlen(mp) - l), "iso");
                }

                printd(3, "Looking up %s in cache\n", mp);
                struct filecache_entry *entry_p = filecache_get(mp);
                if (entry_p)  {
                        if (!entry_p->flags.vsize_resolved) {
                              entry_p->vsize_real_next = next->FileDataEnd;
                              /* If GET_RAR_PACK_SZ() returns 0 keep next size
                               * as is. This will prevent a division by zero
                               * problem later when file is accessed. */
                              entry_p->vsize_next = GET_RAR_PACK_SZ(&next->hdr)
                                      ? (off_t)GET_RAR_PACK_SZ(&next->hdr)
                                      : entry_p->vsize_next;
                              entry_p->flags.vsize_resolved = 1;
                              /*
                               * Check if we might need to compensate for the
                               * 1-byte/2-byte RAR5 (and later?) volume number
                               * in next main archive header.
                               */
                              if (next->hdr.UnpVer >= 50) {
                                      /*
                                       * If base is last or next to last volume
                                       * with one extra byte in header this and
                                       * next volume size have already been
                                       * resolved.
                                       */
                                      if ((entry_p->vno_base - entry_p->vno_first + 1) < 128) {
                                              if (entry_p->stat.st_size >
                                                            (entry_p->vsize_first +
                                                            (entry_p->vsize_next * 
                                                                    (128 - (entry_p->vno_base - entry_p->vno_first + 1)))))
                                                      entry_p->flags.vsize_fixup_needed = 1;
                                      }
                              }
                        }
                        /*
                         * Check if this was a forced/fake entry. In that
                         * case update it with proper stats.
                         */
                        if (entry_p->flags.force_dir) {
                                set_rarstats(entry_p, next, 0);
                                entry_p->flags.force_dir = 0;
                        }
                        goto cache_hit;
                }

                /* Allocate a cache entry for this file */
                printd(3, "Adding %s to cache\n", mp);
                entry_p = filecache_alloc(mp);
                entry_p->rar_p = strdup(arch);
                entry_p->file_p = strdup(next->hdr.FileName);
                entry_p->flags.vsize_resolved = 1; /* Assume sizes will be resolved */
                entry_p->flags.unresolved = 1;

                if (next->LinkTargetFlags & LINK_T_FILECOPY) {
                        struct filecache_entry *e_p;
                        e_p = lookup_filecopy(path, next, rar_root, display);
                        if (e_p) {
                                filecache_copy(e_p, entry_p);
                                /* Preserve stats of original file */
                                set_rarstats(entry_p, next, 0);
                                goto cache_hit;
                        }
                }

                if (next->hdr.Method == FHD_STORING &&
                                !(next->hdr.Flags & LHD_PASSWORD) &&
                                !IS_RAR_DIR(&next->hdr)) {
                        entry_p->flags.raw = 1;
                        if ((d.Flags & MHD_VOLUME)) {   /* volume ? */
                                int len, pos;

                                entry_p->flags.multipart = 1;
                                entry_p->flags.image = IS_IMG(next->hdr.FileName);
                                entry_p->vtype = VTYPE(d.Flags);
                                entry_p->vno_base = get_vformat(arch, entry_p->vtype, &len, &pos);
                                char *tmp = strdup(arch);
                                if (RARVolNameToFirstName_BUGGED(tmp, !entry_p->vtype)) {
                                        __dircache_invalidate_for_file(mp);
                                        filecache_invalidate(mp);
                                        n_files = 0;
                                        free(tmp);
                                        goto out;
                                }
                                entry_p->vno_first = get_vformat(tmp, entry_p->vtype, NULL, NULL);
                                free(tmp);

                                if (len > 0) {
                                        entry_p->vlen = len;
                                        entry_p->vpos = pos;
                                        if (!IS_RAR_DIR(&next->hdr)) {
                                                entry_p->vsize_real_first = next->FileDataEnd;
                                                entry_p->vsize_first = GET_RAR_PACK_SZ(&next->hdr);
                                                /*
                                                 * Assume next volume to hold same amount
                                                 * of data as the first. It will be adjusted
                                                 * later if needed.
                                                 */
                                                entry_p->vsize_next = entry_p->vsize_first;
                                                entry_p->flags.vsize_resolved = 0;
                                        }
                                } else {
                                        entry_p->flags.raw = 0;
                                        entry_p->flags.save_eof =
                                                OPT_SET(OPT_KEY_SAVE_EOF) ? 1 : 0;
                                }
                        } else {
                                entry_p->flags.multipart = 0;
                                entry_p->offset = (next->Offset + next->HeadSize);
                        }
                } else {        /* Folder or Compressed and/or Encrypted */
                        entry_p->flags.raw = 0;
                        if (!IS_RAR_DIR(&next->hdr)) {
                                entry_p->flags.save_eof =
                                        OPT_SET(OPT_KEY_SAVE_EOF) ? 1 : 0;
                                if (next->hdr.Flags & LHD_PASSWORD)
                                        entry_p->flags.encrypted = 1;
                        }
                        /* Check if part of a volume */
                        if (d.Flags & MHD_VOLUME) {
                                entry_p->flags.multipart = 1;
                                entry_p->vtype = VTYPE(d.Flags);
                                /*
                                 * Make sure parent folders are always searched
                                 * from the first volume file since sub-folders
                                 * might actually be placed elsewhere.
                                 */
                                if (RARVolNameToFirstName_BUGGED(entry_p->rar_p, !entry_p->vtype)) {
                                        __dircache_invalidate_for_file(mp);
                                        filecache_invalidate(mp);
                                        n_files = 0;
                                        goto out;
                                }
                        } else {
                                entry_p->flags.multipart = 0;
                        }
                }
                entry_p->method = next->hdr.Method;
                set_rarstats(entry_p, next, 0);

cache_hit:
                __add_filler(path, buffer, mp);
                next = next->next;
        }

out:
        RARFreeListEx(&L);
        RARCloseArchive(hdl);
        free(tmp1);
        pthread_mutex_unlock(&file_access_mutex);

        /* If no files could be processed, throw an error here */
        return !(n_files > 0);
}

#undef DOS_TO_UNIX_PATH
#undef CHRCMP

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int f0(SCANDIR_ARG3 e)
{
        /*
         * We could maybe fallback to using lstat() here if
         * _DIRENT_HAVE_D_TYPE is not defined. But it is probably
         * way to slow to use in filters. That would also require
         * calls to getcwd() etc. since the current path is not
         * known in this context.
         */
#ifdef _DIRENT_HAVE_D_TYPE
        if (e->d_type != DT_UNKNOWN)
                return (!(IS_RAR(e->d_name) && e->d_type == DT_REG) &&
                        !(IS_CBR(e->d_name) && e->d_type == DT_REG) &&
                        !(IS_NNN(e->d_name) && e->d_type == DT_REG) &&
                        !(IS_RXX(e->d_name) && e->d_type == DT_REG));
#endif
        return !IS_RAR(e->d_name) &&
                        !IS_CBR(e->d_name) &&
                        !IS_NNN(e->d_name) &&
                        !IS_RXX(e->d_name);
}

static int f1(SCANDIR_ARG3 e)
{
        /*
         * We could maybe fallback to using lstat() here if
         * _DIRENT_HAVE_D_TYPE is not defined. But it is probably
         * way to slow to use in filters. That would also require
         * calls to getcwd() etc. since the current path is not
         * known in this context.
         */
#ifdef _DIRENT_HAVE_D_TYPE
        if (e->d_type != DT_UNKNOWN)
                return (IS_RAR(e->d_name) || 
                                IS_CBR(e->d_name) ||
                                IS_NNN(e->d_name)) &&
                                e->d_type == DT_REG;
#endif
        return IS_RAR(e->d_name) ||
                        IS_NNN(e->d_name) ||
                        IS_CBR(e->d_name);
}

static int f2(SCANDIR_ARG3 e)
{
        /*
         * We could maybe fallback to using lstat() here if
         * _DIRENT_HAVE_D_TYPE is not defined. But it is probably
         * way to slow to use in filters. That would also require
         * calls to getcwd() etc. since the current path is not
         * known in this context.
         */
#ifdef _DIRENT_HAVE_D_TYPE
        if (e->d_type != DT_UNKNOWN)
                return IS_RXX(e->d_name) && e->d_type == DT_REG;
#endif
        return IS_RXX(e->d_name);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int syncdir_scan(const char *dir, const char *root,
                struct dir_entry_list **next)
{
        struct dirent **namelist;
        unsigned int f;
        int (*filter[]) (SCANDIR_ARG3) = {f1, f2}; /* f0 not needed */
        int error_count = 0;

        ENTER_("%s", dir);

        for (f = 0; f < (sizeof(filter) / sizeof(filter[0])); f++) {
                int final = 0;
                int vno = -1;
                int i = 0;
                int n = scandir(root, &namelist, filter[f], alphasort);
                if (n < 0) {
                        perror("scandir");
                        return -errno;
                }
                while (i < n) {
                        if (f == 1) {
                                /* We know this is .rNN format so this should
                                 * be a bit faster than calling get_vformat().
                                 */
                                const size_t SLEN = strlen(namelist[i]->d_name);
                                if (namelist[i]->d_name[SLEN - 1] == '0' &&
                                    namelist[i]->d_name[SLEN - 2] == '0') {
                                        vno = 1;
                                } else {
                                        ++vno;
                                }
                        } else {
                                int oldvno = vno;
                                int pos;
                                vno = get_vformat(namelist[i]->d_name, 1, /* new style */
                                                  NULL, &pos);
                                if (vno <= oldvno)
                                        vno = 0;
                                else if (vno > oldvno + 1)
                                        vno = oldvno + 1;
                                else
                                        if(i && strncmp(namelist[i]->d_name,
                                                        namelist[i - 1]->d_name,
                                                        pos))
                                                vno = 0;
                        }
                        if (!vno)
                                final = 0;
                        /* We always need to scan at least two volume files */
                        if (!OPT_INT(OPT_KEY_SEEK_LENGTH, 0) ||
                                        vno <= OPT_INT(OPT_KEY_SEEK_LENGTH, 0)) {
                                if (!final) {
                                        char *arch;
                                        ABS_MP(arch, root, namelist[i]->d_name);
                                        if (listrar(dir, next, arch, &final)) {
                                                *next = dir_entry_add(*next,
                                                               namelist[i]->d_name,
                                                               NULL, DIR_E_NRM);
                                                ++error_count;
                                        }
                                }
                        }
                        free(namelist[i]);
                        ++i;
                }
                free(namelist);
        }

        return error_count ? -ENOENT : 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int convert_fake_iso(char *name)
{
        size_t len;

        if (OPT_SET(OPT_KEY_FAKE_ISO)) {
                int l = OPT_CNT(OPT_KEY_FAKE_ISO)
                        ? optdb_find(OPT_KEY_FAKE_ISO, name)
                        : optdb_find(OPT_KEY_IMG_TYPE, name);
                if (!l)
                        return 0;
                len = strlen(name);
                if (l < 3)
                        name = realloc(name, len + 1 + (3 - l));
                strcpy(name + (len - l), "iso");
                return 1;
        }
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int readdir_scan(const char *dir, const char *root,
                struct dir_entry_list **next,
                struct dir_entry_list **next2)
{
        struct dirent **namelist;
        unsigned int f;
        unsigned int f_end;
        int (*filter[]) (SCANDIR_ARG3) = {f0, f1, f2};
        int error_count = 0;

        ENTER_("%s", dir);

        if (*next2) {
                f_end = (sizeof(filter) / sizeof(filter[0]));
        } else {
                /* New RAR files will not be displayed if the cache is in
                 * effect. Optionally the entry list could be scanned for
                 * matching filenames and display only those not already
                 * cached. That would however affect the performance in the
                 * normal case too and currently the choice is simply to
                 * ignore such files. */
                f_end = 1;
        }

        for (f = 0; f < f_end; f++) {
                int final = 0;
                int vno = -1;
                int i = 0;
                int n = scandir(root, &namelist, filter[f], alphasort);
                if (n < 0) {
                        perror("scandir");
                        continue;
                }
                while (i < n) {
                        if (f == 0) {
                                char *tmp = namelist[i]->d_name;
                                char *tmp2 = NULL;
#ifndef _DIRENT_HAVE_D_TYPE
                                char *path;
                                struct stat st;
                                ABS_MP(path, root, tmp);
                                (void)stat(path, &st);
                                if (S_ISREG(st.st_mode)) {
#endif
                                        tmp2 = strdup(tmp);
                                        if (convert_fake_iso(tmp2))
                                                *next = dir_entry_add(*next, tmp2,
                                                               NULL, DIR_E_NRM);
#ifndef _DIRENT_HAVE_D_TYPE
                                }
#endif
                                *next = dir_entry_add(*next, tmp, NULL, DIR_E_NRM);
                                free(tmp2);
                                goto next_entry;
                        }
                        if (f == 2) {
                                /* We know this is .rNN format so this should
                                 * be a bit faster than calling get_vformat().
                                 */
                                const size_t SLEN = strlen(namelist[i]->d_name);
                                if (namelist[i]->d_name[SLEN - 1] == '0' &&
                                    namelist[i]->d_name[SLEN - 2] == '0') {
                                        vno = 1;
                                } else {
                                        ++vno;
                                }
                        } else { 
                                int oldvno = vno;
                                int pos;
                                vno = get_vformat(namelist[i]->d_name, 1, /* new style */
                                                  NULL, &pos);
                                if (vno <= oldvno) 
                                        vno = 0;
                                else if (vno > oldvno + 1) 
                                        vno = oldvno + 1;
                                else 
                                        if(i && strncmp(namelist[i]->d_name,
                                                        namelist[i - 1]->d_name,
                                                        pos))
                                                vno = 0;
                        }
                        if (!vno)
                                final = 0;
                        /* We always need to scan at least two volume files */
                        if (!OPT_INT(OPT_KEY_SEEK_LENGTH, 0) ||
                                        vno <= OPT_INT(OPT_KEY_SEEK_LENGTH, 0)) {
                                if (!final) {
                                        char *arch;
                                        ABS_MP(arch, root, namelist[i]->d_name);
                                        if (listrar(dir, next2, arch, &final)) {
                                                ++error_count;
                                                *next2 = dir_entry_add(*next2,
                                                               namelist[i]->d_name,
                                                               NULL, DIR_E_NRM);
                                        }
                                }
                        }

next_entry:

                        free(namelist[i]);
                        ++i;
                }
                free(namelist);
        }
        return error_count;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int syncdir(const char *path, int force_sync)
{
        ENTER_("%s", path);

        DIR *dp;
        char *root;
        struct dircache_entry *entry_p;
        struct dir_entry_list *dir_list; /* internal list root */
        struct dir_entry_list *next;

        if (force_sync == SYNCDIR_NO_FORCE) {
                pthread_mutex_lock(&dir_access_mutex);
                entry_p = dircache_get(path);
                pthread_mutex_unlock(&dir_access_mutex);
                if (entry_p)
                        return 0;
        } else {
                pthread_mutex_lock(&dir_access_mutex);
                dircache_invalidate(path);
                pthread_mutex_unlock(&dir_access_mutex);
                entry_p = NULL;
        }

        ABS_ROOT(root, path);
        dp = opendir(root);
        if (dp != NULL) {
                int res;

                dir_list = malloc(sizeof(struct dir_entry_list));
                next = dir_list;
                if (!next)
                        return -ENOMEM;
                dir_list_open(next);
                res = syncdir_scan(path, root, &next);
                (void)closedir(dp);
                if (res) {
                        dir_list_free(dir_list);
                        free(dir_list);
                        return res;
                }

                pthread_mutex_lock(&dir_access_mutex);
                entry_p = dircache_alloc(path);
                if (entry_p)
                        entry_p->dir_entry_list = *dir_list;
                pthread_mutex_unlock(&dir_access_mutex);
        }

        return 0;
}

/*
 *****************************************************************************
 *
 ****************************************************************************/
static int syncrar(const char *path)
{
        ENTER_("%s", path);

        struct dircache_entry *entry_p;
        struct dir_entry_list *dir_list; /* internal list root */
        struct dir_entry_list *next;

        pthread_mutex_lock(&dir_access_mutex);
        entry_p = dircache_get(path);
        pthread_mutex_unlock(&dir_access_mutex);
        if (entry_p)
                return 0;

        dir_list = malloc(sizeof(struct dir_entry_list));
        next = dir_list;
        if (!next)
                return -ENOMEM;
        dir_list_open(next);

        int c = 0;
        int final = 0;
        /* We always need to scan at least two volume files */
        int c_end = OPT_INT(OPT_KEY_SEEK_LENGTH, 0);
        c_end = c_end ? c_end + 1 : c_end;
        struct dir_entry_list *arch_next = arch_list_root.next;

        dir_list_open(dir_list);
        while (arch_next) {
                (void)listrar(path, &next, arch_next->entry.name, &final);
                if ((++c == c_end) || final)
                        break;
                arch_next = arch_next->next;
        }

        pthread_mutex_lock(&dir_access_mutex);
        entry_p = dircache_alloc(path);
        if (entry_p)
                entry_p->dir_entry_list = *dir_list;
        pthread_mutex_unlock(&dir_access_mutex);

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_getattr(const char *path, struct stat *stbuf)
{
        ENTER_("%s", path);

        struct filecache_entry *entry_p;
        int res;

        pthread_mutex_lock(&file_access_mutex);
        entry_p = path_lookup(path, stbuf);
        pthread_mutex_unlock(&file_access_mutex);
        if (entry_p) {
                if (entry_p != LOOP_FS_ENTRY) {
                        dump_stat(stbuf);
                        return 0;
                }
                return -ENOENT;
        }

        /*
         * There was a cache miss and the file could not be found locally!
         * This is bad! To make sure the files does not really exist all
         * rar archives need to be scanned for a matching file = slow!
         */
        if (OPT_FILTER(path))
                return -ENOENT;
        char *safe_path = strdup(path);
        res = syncdir(dirname(safe_path), SYNCDIR_NO_FORCE);
        free(safe_path);
        if (res)
                return res;

        pthread_mutex_lock(&file_access_mutex);
        entry_p = filecache_get(path);
        if (entry_p) {
                memcpy(stbuf, &entry_p->stat, sizeof(struct stat));
                pthread_mutex_unlock(&file_access_mutex);
                dump_stat(stbuf);
                return 0;
        }
        pthread_mutex_unlock(&file_access_mutex);

#if RARVER_MAJOR > 4
        int cmd = 0;
        while (file_cmd[cmd]) {
                size_t len_path = strlen(path);
                size_t len_cmd = strlen(file_cmd[cmd]);
                if (len_path > len_cmd &&
                                !strcmp(&path[len_path - len_cmd], file_cmd[cmd])) {
                        char *root;
                        char *real = (char *)path;
                        /* Frome here on the real path is not needed anymore
                         * and adding it to the cache is simply overkil, thus
                         * it is safe to modify it! */
                        real[len_path - len_cmd] = 0;
                        ABS_ROOT(root, real);
                        if (access(root, F_OK)) {
                                if (filecache_get(real)) {
                                        memset(stbuf, 0, sizeof(struct stat));
                                        stbuf->st_mode = S_IFREG | 0644;
                                        return 0;
                                }
                        }
                        break;
                }
                ++cmd;
        }
#endif

        return -ENOENT;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_getattr2(const char *path, struct stat *stbuf)
{
        ENTER_("%s", path);

        int res;

        pthread_mutex_lock(&file_access_mutex);
        if (path_lookup(path, stbuf)) {
                pthread_mutex_unlock(&file_access_mutex);
                dump_stat(stbuf);
                return 0;
        }
        pthread_mutex_unlock(&file_access_mutex);

        /*
         * There was a cache miss! To make sure the file does not really
         * exist the rar archive needs to be scanned for a matching file.
         * This should not happen very frequently unless the contents of
         * the rar archive was actually changed after it was mounted.
         */
        res = syncrar("/");
        if (res)
                return res;

        pthread_mutex_lock(&file_access_mutex);
        struct filecache_entry *entry_p = filecache_get(path);
        if (entry_p) {
                memcpy(stbuf, &entry_p->stat, sizeof(struct stat));
                pthread_mutex_unlock(&file_access_mutex);
                dump_stat(stbuf);
                return 0;
        }
        pthread_mutex_unlock(&file_access_mutex);

#if RARVER_MAJOR > 4
        int cmd = 0;
        while (file_cmd[cmd]) {
                size_t len_path = strlen(path);
                size_t len_cmd = strlen(file_cmd[cmd]);
                if (len_path > len_cmd &&
                                !strcmp(&path[len_path - len_cmd], file_cmd[cmd])) {
                        char *root;
                        char *real = (char *)path;
                        /* Frome here on the real path is not needed anymore
                         * and adding it to the cache is simply overkil, thus
                         * it is safe to modify it! */
                        real[len_path - len_cmd] = 0;
                        ABS_ROOT(root, real);
                        if (filecache_get(real)) {
                                memset(stbuf, 0, sizeof(struct stat));
                                stbuf->st_mode = S_IFREG | 0644;
                                return 0;
                        }
                        break;
                }
                ++cmd;
        }
#endif

        return -ENOENT;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void dump_dir_list(const char *path, void *buffer, fuse_fill_dir_t filler,
                struct dir_entry_list *next)
{
        ENTER_("%s", path);

#if 0
        struct filecache_entry *entry_p;
        int do_inval_cache = 1;
#else
        (void)path;
#endif

        next = next->next;
        while (next) {
                /*
                 * Purge invalid entries from the cache, display the rest.
                 * Collisions are rare but might occur when a file inside
                 * a RAR archives share the same name with a file in the
                 * back-end fs. The latter will prevail in all cases unless
                 * the directory cache is currently in effect.
                 */
                if (next->entry.valid) {
                        filler(buffer, next->entry.name, next->entry.st, 0);
/* TODO: If/when folder cache becomes optional this needs to be revisted */
#if 0
                        if (next->entry.type == DIR_E_RAR)
                                do_inval_cache = 0;
                        else
                                do_inval_cache = 1;
                } else {
                        if (do_inval_cache ||
                            next->entry.type == DIR_E_NRM) { /* Oops! */
                                char *tmp;
                                ABS_MP(tmp, path, next->entry.name);
                                filecache_invalidate(tmp);
                        }
#endif
                }
                next = next->next;
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_opendir(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);

        DIR *dp = NULL;
        char *root;

        if (fs_loop) {
                if (!strcmp(path, fs_loop_mp_root)) {
                        dp = FS_LOOP_ROOT_DP;
                        goto opendir_ok;
                }
        }

        ABS_ROOT(root, path);
        dp = opendir(root);
        if (dp == NULL && errno == ENOENT) {
                if (filecache_get(path))
                        goto opendir_ok;
                return -ENOENT;
        }
        if (dp == NULL)
                return -errno;

opendir_ok:

        FH_SETIO(fi->fh, malloc(sizeof(struct io_handle)));
        if (!FH_ISSET(fi->fh)) {
                if (dp)
                        closedir(dp);
                return -ENOMEM;
        }
        FH_SETTYPE(fi->fh, IO_TYPE_DIR);
        FH_SETENTRY(fi->fh, NULL);
        FH_SETDP(fi->fh, dp);
        FH_SETPATH(fi->fh, strdup(path));

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
                off_t offset, struct fuse_file_info *fi)
{
        ENTER_("%s", (path ? path : ""));

        (void)offset;           /* touch */

        assert(FH_ISSET(fi->fh) && "bad I/O handle");

        struct io_handle *io = FH_TOIO(fi->fh);
        if (io == NULL)
                return -EIO;

        struct dir_entry_list dir_list;               /* internal list root */
        struct dir_entry_list *next = &dir_list;
        struct dir_entry_list *dir_list2 = NULL;      /* internal list root */
        struct dir_entry_list *next2 = dir_list2;

        dir_list_open(next);

        path = path ? path : FH_TOPATH(fi->fh);
        pthread_mutex_lock(&dir_access_mutex);
        struct dircache_entry *entry_p = dircache_get(path);
        if (!entry_p) {
                pthread_mutex_unlock(&dir_access_mutex);
                dir_list2 = malloc(sizeof(struct dir_entry_list));
                if (!dir_list2)
                        return -ENOMEM;
                next2 = dir_list2;
                dir_list_open(dir_list2);
        } else {
                dir_list2 = dir_list_dup(&entry_p->dir_entry_list);
                pthread_mutex_unlock(&dir_access_mutex);
        }

        DIR *dp = FH_TODP(fi->fh);
        if (dp != NULL) {
                char *root;
                if (fs_loop) {
                        if (!strcmp(path, fs_loop_mp_root))
                                goto fill_buff;
                }
                ABS_ROOT(root, path);
                if (readdir_scan(path, root, &next, &next2)) {
                        pthread_mutex_lock(&dir_access_mutex);
                        dircache_invalidate(path);
                        pthread_mutex_unlock(&dir_access_mutex);
                        goto dump_buff_nocache;
                }
                goto dump_buff;
        }

        /* Check if cache is populated */
        if (entry_p)
                goto fill_buff;

        int vol = 0;

        pthread_mutex_lock(&file_access_mutex);
        struct filecache_entry *entry2_p = filecache_get(path);
        if (entry2_p) {
                char *tmp = strdup(entry2_p->rar_p);
                int multipart = entry2_p->flags.multipart;
                short vtype = entry2_p->vtype;
                pthread_mutex_unlock(&file_access_mutex);
                if (multipart) {
                        int final = 0;
                        int vol_end = OPT_INT(OPT_KEY_SEEK_LENGTH, 0);
                        vol_end = vol_end ? vol_end + 1 : vol_end;
                        printd(3, "Search for local directory in %s\n", tmp);
                        while (!listrar(path, &next2, tmp, &final)) {
                                if ((++vol == vol_end) || final) {
                                        free(tmp);
                                        goto fill_buff;
                                }
                                RARNextVolumeName(tmp, !vtype);
                                printd(3, "Search for local directory in %s\n", tmp);
                        }
                } else {
                        if (tmp) {
                                printd(3, "Search for local directory in %s\n", tmp);
                                if (!listrar(path, &next2, tmp, NULL)) {
                                        free(tmp);
                                        goto fill_buff;
                                }
                        }
                }

                free(tmp);
        } else {
                pthread_mutex_unlock(&file_access_mutex);
        }

        if (vol == 0) {
                dir_list_free(&dir_list);
                if (entry_p) {
                        dir_list_free(dir_list2);
                        free(dir_list2);
                }
                return -ENOENT;
        }

fill_buff:

        filler(buffer, ".", NULL, 0);
        filler(buffer, "..", NULL, 0);

dump_buff:

        (void)dir_list_append(&dir_list, dir_list2);
        dir_list_close(&dir_list);

        if (!entry_p) {
                pthread_mutex_lock(&dir_access_mutex);
                entry_p = dircache_alloc(path);
                if (entry_p)
                        entry_p->dir_entry_list = *dir_list2;
                pthread_mutex_unlock(&dir_access_mutex);
        } else {
                dir_list_free(dir_list2);
                free(dir_list2);
        }

        dump_dir_list(path, buffer, filler, &dir_list);
        dir_list_free(&dir_list);

        return 0;

dump_buff_nocache:

        (void)dir_list_append(&dir_list, dir_list2);
        dir_list_close(&dir_list);

        dump_dir_list(path, buffer, filler, &dir_list);
        dir_list_free(&dir_list);
        dir_list_free(dir_list2);
        free(dir_list2);

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_readdir2(const char *path, void *buffer,
                fuse_fill_dir_t filler, off_t offset,
                struct fuse_file_info *fi)
{
        ENTER_("%s", (path ? path : ""));

        (void)offset;           /* touch */

        struct dir_entry_list *dir_list; /* internal list root */

        path = path ? path : FH_TOPATH(fi->fh);
        pthread_mutex_lock(&dir_access_mutex);
        struct dircache_entry *entry_p = dircache_get(path);
        if (!entry_p) {
                int c = 0;
                int final = 0;
                pthread_mutex_unlock(&dir_access_mutex);
                dir_list = malloc(sizeof(struct dir_entry_list));
                struct dir_entry_list *next = dir_list;
                if (!next)
                        return -ENOMEM;

                /* We always need to scan at least two volume files */
                int c_end = OPT_INT(OPT_KEY_SEEK_LENGTH, 0);
                c_end = c_end ? c_end + 1 : c_end;
                struct dir_entry_list *arch_next = arch_list_root.next;

                dir_list_open(next);
                while (arch_next) {
                        (void)listrar(FH_TOPATH(fi->fh), &next,
                                                        arch_next->entry.name,
                                                        &final);
                        if ((++c == c_end) || final)
                                break;
                        arch_next = arch_next->next;
                }
        } else {
                dir_list = dir_list_dup(&entry_p->dir_entry_list);
                pthread_mutex_unlock(&dir_access_mutex);
        }

        filler(buffer, ".", NULL, 0);
        filler(buffer, "..", NULL, 0);

        dir_list_close(dir_list);
        dump_dir_list(FH_TOPATH(fi->fh), buffer, filler, dir_list);

        if (!entry_p) {
                pthread_mutex_lock(&dir_access_mutex);
                entry_p = dircache_alloc(path);
                if (entry_p)
                        entry_p->dir_entry_list = *dir_list;
                pthread_mutex_unlock(&dir_access_mutex);
        } else {
                dir_list_free(dir_list);
                free(dir_list);
        }

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_releasedir(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", (path ? path : ""));

        (void)path;

        struct io_handle *io = FH_TOIO(fi->fh);
        if (io == NULL)
                return -EIO;

        DIR *dp = FH_TODP(fi->fh);
        if (dp && dp != FS_LOOP_ROOT_DP)
                closedir(FH_TODP(fi->fh));
        free(FH_TOPATH(fi->fh));
        free(FH_TOIO(fi->fh));
        FH_ZERO(fi->fh);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *reader_task(void *arg)
{
        struct io_context *op = (struct io_context *)arg;

        printd(4, "Reader thread started (fp:%p)\n", op->fp);

        for (;;) {
                int req;
                struct timespec ts;
                pthread_mutex_lock(&op->rd_req_mutex);
restart:
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 1;
                while (!op->rd_req) {
                        if (pthread_cond_timedwait(&op->rd_req_cond,
                                        &op->rd_req_mutex, &ts) == ETIMEDOUT) {
                                if (fs_terminated) {
                                        pthread_mutex_unlock(&op->rd_req_mutex);
                                        goto out;
                                }
                                goto restart;
                        }
                        continue;
                }
                req = op->rd_req;
                pthread_mutex_unlock(&op->rd_req_mutex);

                printd(4, "Reader thread wakeup (fp:%p)\n", op->fp);
                if (req > RD_SYNC_NOREAD && !feof(op->fp))
                        (void)readTo(op->buf, op->fp, IOB_SAVE_HIST);

                pthread_mutex_lock(&op->rd_req_mutex);
                op->rd_req = RD_IDLE;
                pthread_cond_broadcast(&op->rd_req_cond); /* sync */
                pthread_mutex_unlock(&op->rd_req_mutex);
        }

out:
        printd(4, "Reader thread stopped (fp:%p)\n", op->fp);
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int preload_index(struct io_buf *buf, const char *path)
{
        ENTER_("%s", path);

        /* check for .avi or .mkv */
        if (!IS_AVI(path) && !IS_MKV(path))
                return -1;

        char *r2i;
        ABS_ROOT(r2i, path);
        strcpy(&r2i[strlen(r2i) - 3], "r2i");
        printd(3, "Preloading index for %s\n", r2i);

        buf->idx.data_p = MAP_FAILED;
        int fd = open(r2i, O_RDONLY);
        if (fd == -1) {
                return -1;
        }

#ifdef HAVE_MMAP
        /* Map the file into address space (1st pass) */
        struct idx_head *h = (struct idx_head *)mmap(NULL,
                        sizeof(struct idx_head), PROT_READ, MAP_SHARED, fd, 0);
        if (h == MAP_FAILED || h->magic != R2I_MAGIC) {
                close(fd);
                return -1;
        }

        /* Map the file into address space (2nd pass) */
        buf->idx.data_p = (void *)mmap(NULL, P_ALIGN_(h->size), PROT_READ,
                                                 MAP_SHARED, fd, 0);
        munmap((void *)h, sizeof(struct idx_head));
        if (buf->idx.data_p == MAP_FAILED) {
                close(fd);
                return -1;
        }
        buf->idx.mmap = 1;
#else
        buf->idx.data_p = malloc(sizeof(struct idx_data));
        if (!buf->idx.data_p) {
                buf->idx.data_p = MAP_FAILED;
                return -1;
        }
        NO_UNUSED_RESULT read(fd, buf->idx.data_p, sizeof(struct idx_head));
        buf->idx.mmap = 0;
#endif
        buf->idx.fd = fd;
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int pow_(int b, int n)
{
        int p = 1;
        while (n--)
                p *= b;
        return p;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/

#define LE_BYTES_TO_W32(b) \
        (uint32_t)(*((b)+3) * 16777216 + *((b)+2) * 65537 + *((b)+1) * 256 + *(b))

static int check_avi_type(struct io_context *op)
{
        uint32_t off = 0;
        uint32_t off_end = 0;
        uint32_t len = 0;
        uint32_t first_fc = 0;

        sleep(1);
        if (!(op->buf->data_p[off + 0] == 'R' &&
              op->buf->data_p[off + 1] == 'I' &&
              op->buf->data_p[off + 2] == 'F' &&
              op->buf->data_p[off + 3] == 'F')) {
                return -1;
        }
        off += 8;
        if (!(op->buf->data_p[off + 0] == 'A' &&
              op->buf->data_p[off + 1] == 'V' &&
              op->buf->data_p[off + 2] == 'I' &&
              op->buf->data_p[off + 3] == ' ')) {
                return -1;
        }
        off += 4;
        if (!(op->buf->data_p[off + 0] == 'L' &&
              op->buf->data_p[off + 1] == 'I' &&
              op->buf->data_p[off + 2] == 'S' &&
              op->buf->data_p[off + 3] == 'T')) {
                return -1;
        }
        off += 4;
        len = LE_BYTES_TO_W32(op->buf->data_p + off);

        /* Search ends here */
        off_end = len + 20;

        /* Locate the AVI header and extract frame count. */
        off += 8;
        if (!(op->buf->data_p[off + 0] == 'a' &&
              op->buf->data_p[off + 1] == 'v' &&
              op->buf->data_p[off + 2] == 'i' &&
              op->buf->data_p[off + 3] == 'h')) {
                return -1;
        }
        off += 4;
        len = LE_BYTES_TO_W32(op->buf->data_p + off);

        /* The frame count will be compared with a possible multi-part
         * OpenDML (AVI 2.0) to detect a badly configured muxer. */
        off += 4;
        first_fc = LE_BYTES_TO_W32(op->buf->data_p + off + 16);

        off += len;
        for (; off < off_end; off += len) {
                off += 4;
                len = LE_BYTES_TO_W32(op->buf->data_p + off);
                off += 4;
                if (op->buf->data_p[off + 0] == 'o' &&
                    op->buf->data_p[off + 1] == 'd' &&
                    op->buf->data_p[off + 2] == 'm' &&
                    op->buf->data_p[off + 3] == 'l' &&
                    op->buf->data_p[off + 4] == 'd' &&
                    op->buf->data_p[off + 5] == 'm' &&
                    op->buf->data_p[off + 6] == 'l' &&
                    op->buf->data_p[off + 7] == 'h') {
                        off += 12;
                        /* Check AVI 2.0 frame count */
                        if (first_fc != LE_BYTES_TO_W32(op->buf->data_p + off))
                                return -1;
                        return 0;
                }
        }

        return 0;
}

#undef LE_BYTE_TO_W32

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int extract_rar_file_info(struct filecache_entry *entry_p, struct RARWcb *wcb)
{
        /* Sanity check */
        if (!entry_p->rar_p)
                return 0;

        RAROpenArchiveDataEx d;
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = entry_p->rar_p;
        d.OpenMode = RAR_OM_LIST;
        d.Callback = list_callback;
        d.UserData = (LPARAM)entry_p->rar_p;
        HANDLE hdl = RAROpenArchiveEx(&d);

        /* Check for fault */
        if (d.OpenResult) {
                if (hdl)
                        RARCloseArchive(hdl);
                return 0;
        }

        RARGetFileInfo(hdl, entry_p->file_p, wcb);
        RARCloseArchive(hdl);

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_open(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);

        printd(3, "(%05d) %-8s%s [0x%" PRIu64 "][called from %05d]\n", getpid(),
                        "OPEN", path, fi->fh, fuse_get_context()->pid);
        struct filecache_entry *entry_p;
        char *root;

        errno = 0;
        pthread_mutex_lock(&file_access_mutex);
        entry_p = path_lookup(path, NULL);

        if (entry_p == NULL) {
#if RARVER_MAJOR > 4
                int cmd = 0;
                while (file_cmd[cmd]) {
                        if (!strcmp(&path[strlen(path) - 5], "#info")) {
                                char *tmp = strdup(path);
                                tmp[strlen(path) - 5] = 0;
                                entry_p = path_lookup(tmp, NULL);
                                free(tmp);
                                if (entry_p == NULL ||
                                    entry_p == LOCAL_FS_ENTRY) {
                                        pthread_mutex_unlock(&file_access_mutex);
                                        return -EIO;
                                }
                                break;
                        }
                        ++cmd;
                }
#endif
                if (entry_p == NULL) {
                        pthread_mutex_unlock(&file_access_mutex);
                        return -ENOENT;
                }
                struct io_handle *io = malloc(sizeof(struct io_handle));
                if (!io) {
                        pthread_mutex_unlock(&file_access_mutex);
                        return -EIO;
                }

                struct filecache_entry *e_p = filecache_clone(entry_p);
                pthread_mutex_unlock(&file_access_mutex);
                struct RARWcb *wcb = malloc(sizeof(struct RARWcb));
                memset(wcb, 0, sizeof(struct RARWcb));
                FH_SETIO(fi->fh, io);
                FH_SETTYPE(fi->fh, IO_TYPE_INFO);
                FH_SETBUF(fi->fh, wcb);
                FH_SETPATH(fi->fh, strdup(path));
                fi->direct_io = 1;   /* skip cache */
                extract_rar_file_info(e_p, wcb);
                filecache_freeclone(e_p);
                return 0;
        }
        if (entry_p == LOCAL_FS_ENTRY) {
                /* In case of O_TRUNC it will simply be passed to open() */
                pthread_mutex_unlock(&file_access_mutex);
                ABS_ROOT(root, path);
                return lopen(root, fi);
        }
        if (entry_p->flags.fake_iso) {
                int res;
                struct filecache_entry *e_p = filecache_clone(entry_p);
                pthread_mutex_unlock(&file_access_mutex);
                if (e_p) {
                        ABS_ROOT(root, e_p->file_p);
                        res = lopen(root, fi);
                        if (res == 0) {
                                /* Override defaults */
                                FH_SETTYPE(fi->fh, IO_TYPE_ISO);
                                FH_SETENTRY(fi->fh, e_p);
                                FH_SETPATH(fi->fh, strdup(path));
                        } else {
                                filecache_freeclone(e_p);
                        }
                } else {
                        res = -EIO;
                }
                return res;
        }
        /*
         * For files inside RAR archives open for exclusive write access
         * is not permitted. That implicity includes also O_TRUNC.
         * O_CREAT/O_EXCL is never passed to open() by FUSE so no need to
         * check those.
         */
        if (fi->flags & (O_WRONLY | O_TRUNC)) {
                pthread_mutex_unlock(&file_access_mutex);
                return -EPERM;
        }

        FILE *fp = NULL;
        struct io_buf *buf = NULL;
        struct io_context *op = NULL;
        struct io_handle* io = NULL;
        pid_t pid = 0;

        if (!FH_ISSET(fi->fh)) {
                if (entry_p->flags.raw) {
                        FILE *fp = fopen(entry_p->rar_p, "r");
                        if (fp != NULL) {
                                io = malloc(sizeof(struct io_handle));
                                op = malloc(sizeof(struct io_context));
                                if (!op || !io)
                                        goto open_error;
                                printd(3, "Opened %s\n", entry_p->rar_p);
                                FH_SETIO(fi->fh, io);
                                FH_SETTYPE(fi->fh, IO_TYPE_RAW);
                                FH_SETENTRY(fi->fh, NULL);
                                FH_SETCONTEXT(fi->fh, op);
                                printd(3, "(%05d) %-8s%s [%-16p]\n", getpid(), "ALLOC", path, FH_TOCONTEXT(fi->fh));
                                op->fp = fp;
                                op->pid = 0;
                                op->seq = 0;
                                op->buf = NULL;
                                op->entry_p = NULL;
                                op->pos = 0;
                                op->vno = -1;   /* force a miss 1:st time */
                                if (entry_p->flags.multipart &&
                                                OPT_SET(OPT_KEY_PREOPEN_IMG) &&
                                                entry_p->flags.image) {
                                        if (entry_p->vtype == 1) {  /* New numbering */
                                                op->vno_max =
                                                    pow_(10, entry_p->vlen) - 1;
                                        } else {
                                                 /*
                                                  * Old numbering is more than obscure when
                                                  * it comes to maximum value. Lets assume
                                                  * something high (almost realistic) here.
                                                  * Will probably hit the open file limit
                                                  * anyway.
                                                  */
                                                 op->vno_max = 901;  /* .rar -> .z99 */
                                        }
                                        op->volh = malloc(op->vno_max * sizeof(struct volume_handle));
                                        if (op->volh) {
                                                memset(op->volh, 0, op->vno_max * sizeof(struct volume_handle));
                                                char *tmp = strdup(entry_p->rar_p);
                                                int j = 0;
                                                while (j < op->vno_max) {
                                                        FILE *fp_ = fopen(tmp, "r");
                                                        if (fp_ == NULL)
                                                                break;
                                                        printd(3, "pre-open %s\n", tmp);
                                                        op->volh[j].fp = fp_;
                                                        /*
                                                         * The file position is only a qualified
                                                         * guess. If it is wrong it will be adjusted
                                                         * later.
                                                         */
                                                        op->volh[j].pos = VOL_REAL_SZ(j) -
                                                                        (j ? VOL_NEXT_SZ : VOL_FIRST_SZ);
                                                        printd(3, "SEEK src_off = %" PRIu64 "\n", op->volh[j].pos);
                                                        fseeko(fp_, op->volh[j].pos, SEEK_SET);
                                                        RARNextVolumeName(tmp, !entry_p->vtype);
                                                        ++j;
                                                }
                                                free(tmp);
                                        } else {
                                                printd(1, "Failed to allocate resource (%u)\n", __LINE__);
                                        }
                                } else {
                                        op->volh = NULL;
                                }

                                /*
                                 * Disable flushing the kernel cache of the file contents on
                                 * every open(). This should only be enabled on files, where
                                 * the file data is never changed externally (not through the
                                 * mounted FUSE filesystem).
                                 * Since the file contents will never change this should save
                                 * us from some user space calls!
                                 */
#if 0 /* disable for now (issue #66) */
                                fi->keep_cache = 1;
#endif

                                /*
                                 * Make sure cache entry is filled in completely
                                 * before cloning it
                                 */
                                op->entry_p = filecache_clone(entry_p);
                                if (!op->entry_p)
                                        goto open_error;
                                goto open_end;
                        }

                        goto open_error;
                }

                buf = malloc(P_ALIGN_(sizeof(struct io_buf) + IOB_SZ));
                if (!buf)
                        goto open_error;
                IOB_RST(buf);

                io = malloc(sizeof(struct io_handle));
                op = malloc(sizeof(struct io_context));
                if (!op || !io)
                        goto open_error;
                op->buf = buf;
                op->entry_p = NULL;

                /* Open PIPE(s) and create child process */
                fp = popen_(entry_p, &pid);
                if (fp != NULL) {
                        FH_SETIO(fi->fh, io);
                        FH_SETTYPE(fi->fh, IO_TYPE_RAR);
                        FH_SETENTRY(fi->fh, NULL);
                        FH_SETCONTEXT(fi->fh, op);
                        printd(3, "(%05d) %-8s%s [%-16p]\n", getpid(), "ALLOC",
                                                path, FH_TOCONTEXT(fi->fh));
                        op->seq = 0;
                        op->pos = 0;
                        op->fp = fp;
                        op->pid = pid;
                        printd(4, "PIPE %p created towards child %d\n",
                                                op->fp, pid);

                        pthread_mutex_init(&op->rd_req_mutex, NULL);
                        pthread_cond_init(&op->rd_req_cond, NULL);
                        op->rd_req = RD_IDLE;

                        /*
                         * The below will take precedence over keep_cache.
                         * This flag will allow the filesystem to bypass the page cache using
                         * the "direct_io" flag.  This is not the same as O_DIRECT, it's
                         * dictated by the filesystem not the application.
                         * Since compressed archives might sometimes require fake data to be
                         * returned in read requests, a cache might cause the same faulty
                         * information to be propagated to sub-sequent reads. Setting this
                         * flag will force _all_ reads to enter the filesystem.
                         */
#if 0 /* disable for now */
                        if (entry_p->flags.direct_io)
                                fi->direct_io = 1;
#endif

                        /* Create reader thread */
                        if (pthread_create(&op->thread, &thread_attr, reader_task, (void *)op))
                                goto open_error;
                        if (sync_thread_noread(op))
                                goto open_error;

                        buf->idx.data_p = MAP_FAILED;
                        buf->idx.fd = -1;
                        if (!preload_index(buf, path)) {
                                entry_p->flags.save_eof = 0;
                                entry_p->flags.direct_io = 0;
                                fi->direct_io = 0;
                        } else {
                                /* Was the file removed ? */
                                if (OPT_SET(OPT_KEY_SAVE_EOF) && !entry_p->flags.save_eof) {
                                        entry_p->flags.save_eof = 1;
                                        entry_p->flags.avi_tested = 0;
                                }
                        }

                        if (entry_p->flags.save_eof && !entry_p->flags.avi_tested) {
                                if (check_avi_type(op))
                                        entry_p->flags.save_eof = 0;
                                entry_p->flags.avi_tested = 1;
                        }

#ifdef DEBUG_READ
                        char out_file[32];
                        sprintf(out_file, "%s.%d", "output", pid);
                        op->dbg_fp = fopen(out_file, "w");
#endif
                        /*
                         * Make sure cache entry is filled in completely
                         * before cloning it
                         */
                        op->entry_p = filecache_clone(entry_p);
                        if (!op->entry_p)
                                goto open_error;
                        goto open_end;
                }
        }

open_error:
        pthread_mutex_unlock(&file_access_mutex);
        if (fp)
                pclose_(fp, pid);
        if (op) {
                if (op->entry_p)
                        filecache_freeclone(op->entry_p);
                free(op);
        }
        if (buf)
                free(buf);

        /*
         * This is the best we can return here. So many different things
         * might go wrong and errno can actually be set to something that
         * FUSE is accepting and thus proceeds with next operation!
         */
        printd(1, "open: I/O error\n");
        return -EIO;

open_end:
        FH_SETPATH(fi->fh, strdup(path));
        op->entry_p->flags.check_atime = 1;
        pthread_mutex_unlock(&file_access_mutex);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int access_chk(const char *path, int new_file)
{
        struct filecache_entry *e;

        if (fs_loop) {
                if (!strncmp(path, fs_loop_mp_base, fs_loop_mp_base_len) ||
                    !strcmp(path, fs_loop_mp_root))
                        return 1;
        }

        /*
         * To return a more correct fault code if an attempt is
         * made to create/remove a file in a RAR folder, a cache lookup
         * will tell if operation should be permitted or not.
         * Simply, if the file/folder is in the cache and cannot be
         * found/accessed natively, forget it!
         *   This works fine in most cases but it does not work for some
         * specific programs like 'touch'. A 'touch' may result in a
         * getattr() callback even if -EPERM is returned by open() which
         * will eventually render a "No such file or directory" type of
         * error/message.
         */
        char *real;
        if (new_file) {
                char *p = strdup(path); /* In case p is destroyed by dirname() */
                e = filecache_get(dirname(p));
                ABS_ROOT(real, p);
                free(p);
        } else {
                e = filecache_get(path);
                ABS_ROOT(real, path);
        }
        return e && !e->flags.unresolved ? 1 : 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *rar2_init(struct fuse_conn_info *conn)
{
        ENTER_();

        (void)conn;             /* touch */

        filecache_init();
        dircache_init();
        iobuffer_init();
        sighandler_init();

        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void rar2_destroy(void *data)
{
        ENTER_();

        (void)data;             /* touch */

        iobuffer_destroy();
        dircache_destroy();
        filecache_destroy();
        sighandler_destroy();
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_flush(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", (path ? path : ""));

        (void)path;             /* touch */

        printd(3, "(%05d) %s [%-16p][called from %05d]\n", getpid(),
               "FLUSH", FH_TOCONTEXT(fi->fh), fuse_get_context()->pid);
        return lflush(fi);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_readlink(const char *path, char *buf, size_t buflen)
{
        ENTER_("%s", path);

        struct filecache_entry *entry_p;

        if (!buflen)
                return -EINVAL;

        entry_p = path_lookup(path, NULL);
        if (entry_p && entry_p != LOCAL_FS_ENTRY) {
                if (entry_p->link_target_p)
                        strncpy(buf, entry_p->link_target_p, buflen - 1);
                else
                        return -EIO;
        } else {
                char *tmp;
                ABS_ROOT(tmp, path);
                buflen = readlink(tmp, buf, buflen - 1);
                if ((ssize_t)buflen == -1) /* readlink(2) returns ssize_t */
                        return -errno;
        }

        buf[buflen] = 0;
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_symlink(const char *from, const char *to)
{
        ENTER_("%s", from);
        if (!access_chk(to, 1)) {
                char *root;
                ABS_ROOT(root, to);
                if (!symlink(from, root))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_statfs(const char *path, struct statvfs *vfs)
{
        ENTER_("%s", path);

        (void)path;             /* touch */

        if (!statvfs(OPT_STR2(OPT_KEY_SRC, 0), vfs))
                return 0;
        return -errno;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_release(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", (path ? path : ""));

        (void)path;             /* touch */

        if (!FH_ISSET(fi->fh))
                return 0;

        printd(3, "(%05d) %s [0x%-16" PRIx64 "]\n", getpid(), "RELEASE", fi->fh);

        if (FH_TOIO(fi->fh)->type == IO_TYPE_RAR ||
                        FH_TOIO(fi->fh)->type == IO_TYPE_RAW) {
                struct io_context *op = FH_TOCONTEXT(fi->fh);
                free(FH_TOPATH(fi->fh));
                if (op->fp) {
                        if (op->entry_p->flags.raw) {
                                if (op->volh) {
                                        int j;
                                        for (j = 0; j < op->vno_max; j++) {
                                                if (op->volh[j].fp)
                                                        fclose(op->volh[j].fp);
                                        }
                                        free(op->volh);
                                }
                                printd(3, "Closing file handle %p\n", op->fp);
                                fclose(op->fp);
                        } else {
                                if (!pthread_cancel(op->thread))
                                        pthread_join(op->thread, NULL);

                                pthread_cond_destroy(&op->rd_req_cond);
                                pthread_mutex_destroy(&op->rd_req_mutex);

                                if (pclose_(op->fp, op->pid))
                                        printd(4, "child closed abnormally\n");
                                printd(4, "PIPE %p closed towards child %05d\n",
                                               op->fp, op->pid);
#ifdef DEBUG_READ
                                fclose(op->dbg_fp);
#endif
                        }
                }
                printd(3, "(%05d) %s [0x%-16" PRIx64 "]\n", getpid(), "FREE", fi->fh);
                if (op->buf) {
                        /* XXX clean up */
#ifdef HAVE_MMAP
                        if (op->buf->idx.data_p != MAP_FAILED &&
                                        op->buf->idx.mmap)
                                munmap((void *)op->buf->idx.data_p,
                                                P_ALIGN_(op->buf->idx.data_p->head.size));
#endif
                        if (op->buf->idx.data_p != MAP_FAILED &&
                                        !op->buf->idx.mmap)
                                free(op->buf->idx.data_p);
                        if (op->buf->idx.fd != -1)
                                close(op->buf->idx.fd);
                        free(op->buf);
                }
                filecache_freeclone(op->entry_p);
                free(op);
                free(FH_TOIO(fi->fh));
                FH_ZERO(fi->fh);
        } else {
                return lrelease(fi);
        }

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_read(const char *path, char *buffer, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        int res;
        struct io_handle *io;
        assert(FH_ISSET(fi->fh) && "bad I/O handle");

        (void)path;             /* touch */

        io = FH_TOIO(fi->fh);
        if (!io)
               return -EIO;

        ENTER_("size=%zu, offset=%" PRIu64 ", fh=%" PRIu64, size, offset, fi->fh);

        if (io->type == IO_TYPE_NRM) {
                res = lread(buffer, size, offset, fi);
        } else if (io->type == IO_TYPE_ISO) {
                res = lread(buffer, size, offset, fi);
        } else if (io->type == IO_TYPE_RAW) {
                res = lread_raw(buffer, size, offset, fi);
        } else if (io->type == IO_TYPE_INFO) {
                res = lread_info(buffer, size, offset, fi);
        } else if (io->type == IO_TYPE_RAR) {
                res = lread_rar(buffer, size, offset, fi);
        } else
                return -EIO;
        return res;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_truncate(const char *path, off_t offset)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!truncate(root, offset))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_write(const char *path, const char *buffer, size_t size,
                off_t offset, struct fuse_file_info *fi)
{
        ssize_t n;

        ENTER_("%s", (path ? path : ""));

        (void)path;             /* touch */

        n = pwrite(FH_TOFD(fi->fh), buffer, size, offset);
        return n >= 0 ? n : -errno;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_chmod(const char *path, mode_t mode)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!chmod(root, mode))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_chown(const char *path, uid_t uid, gid_t gid)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!chown(root, uid, gid))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
        ENTER_("%s", path);
        /* Only allow creation of "regular" files this way */
        if (S_ISREG(mode)) {
                if (!access_chk(path, 1)) {
                        char *root;
                        ABS_ROOT(root, path);
                        int fd = creat(root, mode);
                        if (fd == -1)
                                return -errno;
                        if (!FH_ISSET(fi->fh)) {
                                struct io_handle *io =
                                        malloc(sizeof(struct io_handle));
                                /*
                                 * Does not really matter what is returned in
                                 * case of failure as long as it is not 0.
                                 * Returning anything but 0 will avoid the
                                 * _release() call but will still create the
                                 * file when called from e.g. 'touch'.
                                 */
                                if (!io) {
                                        close(fd);
                                        return -EIO;
                                }
                                FH_SETIO(fi->fh, io);
                                FH_SETTYPE(fi->fh, IO_TYPE_NRM);
                                FH_SETENTRY(fi->fh, NULL);
                                FH_SETFD(fi->fh, fd);
                        }
                        __dircache_invalidate_for_file(path);
                        return 0;
                }
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_eperm_stub()
{
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_rename(const char *oldpath, const char *newpath)
{
        ENTER_("%s", oldpath);
        /* We can not move things out of- or from RAR archives */
        if (!access_chk(newpath, 0) && !access_chk(oldpath, 0)) {
                char *oldroot;
                char *newroot;
                ABS_ROOT(oldroot, oldpath);
                ABS_ROOT(newroot, newpath);
                if (!rename(oldroot, newroot)) {
                        __dircache_invalidate_for_file(oldpath);
                        return 0;
                }
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_mknod(const char *path, mode_t mode, dev_t dev)
{
        ENTER_("%s", path);
        if (!access_chk(path, 1)) {
                char *root;
                ABS_ROOT(root, path);
                if (!mknod(root, mode, dev))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_unlink(const char *path)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!unlink(root)) {
                        __dircache_invalidate_for_file(path);
                        return 0;
                }
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_mkdir(const char *path, mode_t mode)
{
        ENTER_("%s", path);
        if (!access_chk(path, 1)) {
                char *root;
                ABS_ROOT(root, path);
                if (!mkdir(root, mode))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_rmdir(const char *path)
{
        ENTER_("%s", path);
        if (!access_chk(path, 0)) {
                char *root;
                ABS_ROOT(root, path);
                if (!rmdir(root)) {
                        dircache_invalidate(path);
                        return 0;
                }
                return -errno;
        }
        return -EPERM;
}

#if defined( HAVE_UTIMENSAT ) && defined( AT_SYMLINK_NOFOLLOW )
/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_utimens(const char *path, const struct timespec ts[2])
{
        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                int res;
                char *root;
                ABS_ROOT(root, path);
                /* don't use utime/utimes since they follow symlinks */
                res = utimensat(0, root, ts, AT_SYMLINK_NOFOLLOW);
                if (res == -1)
                        return -errno;
                return 0;
        }
        return -EPERM;
}
#endif

#ifdef HAVE_SETXATTR

static const char *xattr[4] = {
        "user.rar2fs.cache_method",
        "user.rar2fs.cache_flags",
        "user.rar2fs.cache_dir_hash",
        NULL
};
#define XATTR_CACHE_METHOD 0
#define XATTR_CACHE_FLAGS 1

/*!
*****************************************************************************
*
****************************************************************************/
#ifdef XATTR_ADD_OPT
static int rar2_getxattr(const char *path, const char *name, char *value,
                size_t size, uint32_t position)
#else
static int rar2_getxattr(const char *path, const char *name, char *value,
                size_t size)
#endif
{
        struct filecache_entry *e_p;
        int xattr_no;
        size_t len;

        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                char *tmp;
                ABS_ROOT(tmp, path);
#ifdef XATTR_ADD_OPT
                size = getxattr(tmp, name, value, size, position,
                                        XATTR_NOFOLLOW);
#else
                size = lgetxattr(tmp, name, value, size);
#endif
                if ((ssize_t)size == -1)
                        return -errno;
                return size;
        }

        e_p = filecache_get(path);
        if (e_p == NULL)
                return -ENOTSUP;

        if (!strcmp(name, xattr[XATTR_CACHE_METHOD]) &&
                        !S_ISDIR(e_p->stat.st_mode)) {
                len = sizeof(uint16_t);
                xattr_no = XATTR_CACHE_METHOD;
        } else if (!strcmp(name, xattr[XATTR_CACHE_FLAGS])) {
                len = sizeof(uint32_t);
                xattr_no = XATTR_CACHE_FLAGS;
        } else {
                /*
                 * According to Linux man page, ENOATTR is defined to be a
                 * synonym for ENODATA in <attr/xattr.h>. But <attr/xattr.h>
                 * does not seem to exist on that many systems, so return
                 * -ENODATA here instead.
                 */
                return -ENODATA;
        }

        if (size) {
                if (size < len)
                        return -ERANGE;
                switch (xattr_no) {
                case XATTR_CACHE_METHOD:
                        *(uint16_t*)value = htons(e_p->method - FHD_STORING);
                        break;
                case XATTR_CACHE_FLAGS:
                        *(uint32_t*)value = htonl(e_p->flags_uint32);
                        break;
                }
        }
        return len;
}

/*!
*****************************************************************************
*
****************************************************************************/
static int rar2_listxattr(const char *path, char *list, size_t size)
{
        int i;
        size_t len;
        struct filecache_entry *e_p;

        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                char *tmp;
                ABS_ROOT(tmp, path);
#ifdef XATTR_ADD_OPT
                size = listxattr(tmp, list, size, XATTR_NOFOLLOW);
#else
                size = llistxattr(tmp, list, size);
#endif
                if ((ssize_t)size == -1)
                        return -errno;
                return size;
        }

        e_p = filecache_get(path);
        if (e_p == NULL)
                return -ENOTSUP;

        i = 0;
        len = 0;
        while (xattr[i]) {
                if (!S_ISDIR(e_p->stat.st_mode) ||
                                i != XATTR_CACHE_METHOD)
                        len += (strlen(xattr[i]) + 1);
                ++i;
        }
        if (size) {
                i = 0;
                if (size < len)
                        return -ERANGE;
                while (xattr[i]) {
                        if (!S_ISDIR(e_p->stat.st_mode) ||
                                        i != XATTR_CACHE_METHOD) {
                                strcpy(list, xattr[i]);
                                list += (strlen(list) + 1);
                        }
                        ++i;
                }
        }
        return len;
}

/*!
*****************************************************************************
*
****************************************************************************/
#ifdef XATTR_ADD_OPT
static int rar2_setxattr(const char *path, const char *name, const char *value,
                size_t size, int flags, uint32_t position)
#else
static int rar2_setxattr(const char *path, const char *name, const char *value,
                size_t size, int flags)
#endif
{
        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                char *tmp;
                ABS_ROOT(tmp, path);
#ifdef XATTR_ADD_OPT
                size = setxattr(tmp, name, value, size, position,
                                        flags | XATTR_NOFOLLOW);
#else
                size = lsetxattr(tmp, name, value, size, flags);
#endif
                if ((ssize_t)size == -1)
                        return -errno;
                return 0;
        }
        return -ENOTSUP;
}

/*!
*****************************************************************************
*
****************************************************************************/
static int rar2_removexattr(const char *path, const char *name)
{
        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                int res;
                char *tmp;
                ABS_ROOT(tmp, path);
#ifdef XATTR_ADD_OPT
                res = removexattr(tmp, name, XATTR_NOFOLLOW);
#else
                res = lremovexattr(tmp, name);
#endif
                if (res == -1)
                        return -errno;
                return 0;
        }
        return -ENOTSUP;
}
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void usage(char *prog)
{
        const char *P_ = basename(prog);
        printf("Usage: %s source mountpoint [options]\n", P_);
        printf("Try `%s -h' or `%s --help' for more information.\n", P_, P_);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int64_t get_blkdev_size(struct stat *st)
{
#ifdef __linux
        struct stat st2;
        char buf[PATH_MAX];
	size_t len;
	int fd;

	snprintf(buf, sizeof(buf), "/sys/dev/block/%d:%d/loop/backing_file",
		 major(st->st_rdev), minor(st->st_rdev));

	fd = open(buf, O_RDONLY);
	if (fd < 0)
		return 0;

	len = read(fd, buf, PATH_MAX);
	close(fd);
	if (len < 2)
		return 0;

	buf[len - 1] = '\0';
        (void)stat(buf, &st2);
        return st2.st_size;
#else
        (void)st;  /* touch */
        return 0;
#endif
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int check_paths(const char *prog, char *src_path_in, char *dst_path_in,
                char **src_path_out, char **dst_path_out, int verbose)
{
        char p1[PATH_MAX];
        char p2[PATH_MAX];
        char *a1 = realpath(src_path_in, p1);
        char *a2 = realpath(dst_path_in, p2);
        if (!a1 || !a2 || !strcmp(a1, a2)) {
                if (verbose)
                        printf("%s: invalid source and/or mount point\n", prog);
                return -1;
        }
        dir_list_open(arch_list);
        struct stat st;
        (void)stat(a1, &st);
        mount_type = S_ISDIR(st.st_mode) ? MOUNT_FOLDER : MOUNT_ARCHIVE;

        /* Check for block special file */
        if (mount_type == MOUNT_ARCHIVE && S_ISBLK(st.st_mode))
                blkdev_size = get_blkdev_size(&st);

        /* Check path type(s), destination path *must* be a folder */
        (void)stat(a2, &st);
        if (!S_ISDIR(st.st_mode)) {
                if (verbose)
                        printf("%s: invalid source and/or mount point\n", prog);
                return -1;
        }

        /* Check file collection at archive mount */
        if (mount_type == MOUNT_ARCHIVE) {
                int ret = collect_files(a1, arch_list);
                if (ret == -EPERM) {
                        if (verbose)
                                printf("%s: bad or missing password\n", prog);
                        return -1;
                } else if (ret <= 0) {
                        if (verbose)
                                printf("%s: invalid source and/or mount point\n",
                                                prog);
                        return -1;
                }
        }

        /* Do not try to use 'a1' after this call since dirname() will destroy it! */
        *src_path_out = mount_type == MOUNT_FOLDER
                ? strdup(a1) : strdup(dirname(a1));
        *dst_path_out = strdup(a2);

        /* Detect a possible file system loop */
        if (mount_type == MOUNT_FOLDER) {
                if (!strncmp(*src_path_out, *dst_path_out,
                                        strlen(*src_path_out))) {
                        if ((*dst_path_out)[strlen(*src_path_out)] == '/') {
                                memcpy(&fs_loop_mp_stat, &st, sizeof(struct stat));
                                fs_loop = 1;
                                char *safe_path = strdup(dst_path_in);
                                char *tmp = basename(safe_path);
                                fs_loop_mp_root = malloc(strlen(tmp) + 2);
                                sprintf(fs_loop_mp_root, "/%s", tmp);
                                free(safe_path);
                                fs_loop_mp_base = malloc(strlen(fs_loop_mp_root) + 2);
                                sprintf(fs_loop_mp_base, "%s/", fs_loop_mp_root);
                                fs_loop_mp_base_len = strlen(fs_loop_mp_base);
                        }
                }
        }

        /* Do not close the list since it could re-order the entries! */
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int check_iob(char *bname, int verbose)
{
        unsigned int bsz = OPT_INT(OPT_KEY_BUF_SIZE, 0);
        unsigned int hsz = OPT_INT(OPT_KEY_HIST_SIZE, 0);
        if ((OPT_SET(OPT_KEY_BUF_SIZE) && !bsz) || (bsz & (bsz - 1)) ||
                        (OPT_SET(OPT_KEY_HIST_SIZE) && (hsz > 75))) {
                if (verbose)
                        usage(bname);
                return -1;
        }
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
#ifdef HAVE_STATIC_UNRAR
#define check_libunrar(x) 0
#else
static int check_libunrar(int verbose)
{
        if (RARGetDllVersion() != RAR_DLL_VERSION) {
                if (verbose) {
                        if (RARVER_BETA) {
                                printf("libunrar.so (v%d.%d beta %d) or compatible library not found\n",
                                       RARVER_MAJOR, RARVER_MINOR, RARVER_BETA);
                        } else {
                                printf("libunrar.so (v%d.%d) or compatible library not found\n",
                                       RARVER_MAJOR, RARVER_MINOR);
                        }
                }
                return -1;
        }
        return 0;
}
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int check_libfuse(int verbose)
{
#ifdef HAVE_FUSE_VERSION
        if (fuse_version() < FUSE_VERSION) {
                if (verbose)
                        printf("libfuse.so.%d.%d or compatible library not found\n",
                               FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);
                return -1;
        }
#endif
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/

/* Mapping of static/non-configurable FUSE file system operations. */
static struct fuse_operations rar2_operations = {
        .init = rar2_init,
        .statfs = rar2_statfs,
#if defined( HAVE_UTIMENSAT ) && defined( AT_SYMLINK_NOFOLLOW )
        .utimens = rar2_utimens,
#endif
        .destroy = rar2_destroy,
        .open = rar2_open,
        .release = rar2_release,
        .opendir = rar2_opendir,
        .releasedir = rar2_releasedir,
        .read = rar2_read,
        .flush = rar2_flush,
        .readlink = rar2_readlink,
#ifdef HAVE_SETXATTR
        .getxattr = rar2_getxattr,
        .setxattr = rar2_setxattr,
        .listxattr = rar2_listxattr,
        .removexattr = rar2_removexattr,
#endif
#if FUSE_MAJOR_VERSION == 2 && FUSE_MINOR_VERSION > 7
        .flag_nullpath_ok = 1,
#endif
#if FUSE_MAJOR_VERSION > 2 || (FUSE_MAJOR_VERSION == 2 && FUSE_MINOR_VERSION >= 9)
        .flag_nopath = 1,
#endif
};

struct work_task_data {
        struct fuse *fuse;
        int mt;
        volatile int work_task_exited;
        int status;
};

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *work_task(void *data)
{
        struct work_task_data *wdt = (struct work_task_data *)data;
        wdt->status = wdt->mt ? fuse_loop_mt(wdt->fuse) : fuse_loop(wdt->fuse);
        wdt->work_task_exited = 1;
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void scan_fuse_new_args(struct fuse_args *args)
{
        const char *match_w_arg = "subtype=rar2fs";
        int i;

        /* The loop is probably overkill, but lets be safe instead of
           hardcoding a fixed index of 2. */
        for (i = 0; i < args->argc; i++) {
                char *needle;

                /* First check for match with the internally added option */
                if ((needle = strstr(args->argv[i], match_w_arg))) {
                        if (needle != args->argv[i]) {
                                --needle;
                                if (*needle != ',')
                                        needle = NULL;
                        }
                        if (needle) {
                                strcpy(needle, needle + strlen(match_w_arg) + 1);
                                break;
                        }
                }
       }
}

/* stdio backups */
static int stdout_ = 0;
static int stderr_ = 0;

/*!
 *****************************************************************************
 * This function is not thread safe!
 ****************************************************************************/
static void block_stdio()
{
        /* Checking one is enough here */
        if (!stdout_) {
                int new = open("/dev/null", O_WRONLY);
                if (new) {
                        fflush(stdout);
                        fflush(stderr);
                        stdout_ = dup(fileno(stdout));
                        stderr_ = dup(fileno(stderr));
                        dup2(new, fileno(stderr));
                        dup2(new, fileno(stdout));
                        close(new);
                }
        }
}

/*!
 *****************************************************************************
 * This function is not thread safe!
 ****************************************************************************/
static void release_stdio()
{
        /* Checking one is enough here */
        if (stdout_) {
                fflush(stdout);
                fflush(stderr);
                dup2(stdout_, fileno(stdout));
                dup2(stderr_, fileno(stderr));
                close(stderr_);
                close(stdout_);
                stdout_ = 0;
                stderr_ = 0;
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int work(struct fuse_args *args)
{
        struct work_task_data wdt;

        /* For portability, explicitly create threads in a joinable state */
        pthread_attr_init(&thread_attr);
        pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);

#if defined ( HAVE_SCHED_SETAFFINITY ) && defined ( HAVE_CPU_SET_T )
        cpu_set_t cpu_mask_saved;
        if (OPT_SET(OPT_KEY_NO_SMP)) {
                cpu_set_t cpu_mask;
                CPU_ZERO(&cpu_mask);
                CPU_SET(0, &cpu_mask);
                if (sched_getaffinity(0, sizeof(cpu_set_t), &cpu_mask_saved))
                        perror("sched_getaffinity");
                else if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_mask))
                        perror("sched_setaffinity");
        }
#endif

        /* The below callbacks depend on mount type */
        if (mount_type == MOUNT_FOLDER) {
                rar2_operations.getattr         = rar2_getattr;
                rar2_operations.readdir         = rar2_readdir;
                rar2_operations.create          = rar2_create;
                rar2_operations.rename          = rar2_rename;
                rar2_operations.mknod           = rar2_mknod;
                rar2_operations.unlink          = rar2_unlink;
                rar2_operations.mkdir           = rar2_mkdir;
                rar2_operations.rmdir           = rar2_rmdir;
                rar2_operations.write           = rar2_write;
                rar2_operations.truncate        = rar2_truncate;
                rar2_operations.chmod           = rar2_chmod;
                rar2_operations.chown           = rar2_chown;
                rar2_operations.symlink         = rar2_symlink;
        } else {
                rar2_operations.getattr         = rar2_getattr2;
                rar2_operations.readdir         = rar2_readdir2;
                rar2_operations.create          = (void *)rar2_eperm_stub;
                rar2_operations.rename          = (void *)rar2_eperm_stub;
                rar2_operations.mknod           = (void *)rar2_eperm_stub;
                rar2_operations.unlink          = (void *)rar2_eperm_stub;
                rar2_operations.mkdir           = (void *)rar2_eperm_stub;
                rar2_operations.rmdir           = (void *)rar2_eperm_stub;
                rar2_operations.write           = (void *)rar2_eperm_stub;
                rar2_operations.truncate        = (void *)rar2_eperm_stub;
                rar2_operations.chmod           = (void *)rar2_eperm_stub;
                rar2_operations.chown           = (void *)rar2_eperm_stub;
                rar2_operations.symlink         = (void *)rar2_eperm_stub;
        }

        struct fuse *f = NULL;
        struct fuse_chan *ch = NULL;
        struct fuse_session *se = NULL;
        pthread_t t;
        char *mp;
        int mt = 0;
        int fg = 0;

        /* This is doing more or less the same as fuse_setup(). */
        if (!fuse_parse_cmdline(args, &mp, &mt, &fg)) {
              ch = fuse_mount(mp, args);
              if (ch) {
                      /* Avoid any output from the initial attempt */
                      block_stdio();
                      f = fuse_new(ch, args, &rar2_operations,
                                        sizeof(rar2_operations), NULL);
                      release_stdio();
                      if (f == NULL) {
                              /* Check if the operation might succeed the
                               * second time after having massaged the
                               * arguments. */
                              (void)scan_fuse_new_args(args);
                              f = fuse_new(ch, args, &rar2_operations,
                                        sizeof(rar2_operations), NULL);
                      }
                      if (f == NULL) {
                              fuse_unmount(mp, ch);
                      } else {
                              syslog(LOG_DEBUG, "mounted %s\n", mp);
                              se = fuse_get_session(f);
                              fuse_set_signal_handlers(se);
                              fuse_daemonize(fg);
                      }
              }
        }

        if (f == NULL)
                return -1;

        wdt.fuse = f;
        wdt.mt = mt;
        wdt.work_task_exited = 0;
        wdt.status = 0;
        pthread_create(&t, &thread_attr, work_task, (void *)&wdt);

        /*
         * This is a workaround for an issue with fuse_loop() that does
         * not always release properly at reception of SIGINT.
         * But this is really what we want since this thread should not
         * block blindly without some user control.
         */
        while (!fuse_exited(f) && !wdt.work_task_exited) {
                sleep(1);
                ++rar2_ticks;
        }
        if (!wdt.work_task_exited)
                pthread_kill(t, SIGINT);        /* terminate nicely */

        fs_terminated = 1;
        pthread_join(t, NULL);

        /* This is doing more or less the same as fuse_teardown(). */
        fuse_remove_signal_handlers(se);
        fuse_unmount(mp, ch);
        syslog(LOG_DEBUG, "unmounted %s\n", mp);
        fuse_destroy(f);
        free(mp);

#if defined ( HAVE_SCHED_SETAFFINITY ) && defined ( HAVE_CPU_SET_T )
        if (OPT_SET(OPT_KEY_NO_SMP)) {
                if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_mask_saved))
                        perror("sched_setaffinity");
        }
#endif

        pthread_attr_destroy(&thread_attr);

        return wdt.status;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void print_version()
{
        char src_rev[16];
#ifdef GITREV_
        snprintf(src_rev, 16, "-git%x", GITREV_);
#else
        src_rev[0] = '\0';
#endif
        printf("rar2fs v%u.%u.%u%s (DLL version %d)    Copyright (C) 2009 Hans Beckerus\n",
               RAR2FS_MAJOR_VER,
               RAR2FS_MINOR_VER, RAR2FS_PATCH_LVL,
               src_rev,
               RARGetDllVersion());
        printf("This program comes with ABSOLUTELY NO WARRANTY.\n"
               "This is free software, and you are welcome to redistribute it under\n"
               "certain conditions; see <http://www.gnu.org/licenses/> for details.\n");
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void print_help()
{
        printf("\nrar2fs options:\n");
        printf("    --img-type=E1[;E2...]   additional image file type extensions beyond the default [.iso;.img;.nrg]\n");
        printf("    --show-comp-img\t    show image file types also for compressed archives\n");
        printf("    --preopen-img\t    prefetch volume file descriptors for image file types\n");
        printf("    --fake-iso[=E1[;E2...]] fake .iso extension for specified image file types\n");
        printf("    --exclude=F1[;F2...]    exclude file filter\n");
        printf("    --seek-length=n\t    set number of volume files that are traversed in search for headers [0=All]\n");
#ifndef USE_STATIC_IOB_
        printf("    --iob-size=n\t    I/O buffer size in 'power of 2' MiB (1,2,4,8, etc.) [4]\n");
        printf("    --hist-size=n\t    I/O buffer history size as a percentage (0-75) of total buffer size [50]\n");
#endif
        printf("    --save-eof\t\t    force creation of .r2i files (end-of-file chunk)\n");
        printf("    --no-lib-check\t    disable validation of library version(s)\n");
        printf("    --no-expand-cbr\t    do not expand comic book RAR archives\n");
#if defined ( HAVE_SCHED_SETAFFINITY ) && defined ( HAVE_CPU_SET_T )
        printf("    --no-smp\t\t    disable SMP support (bind to CPU #0)\n");
#endif
        printf("    --relatime\t\t    update file access times relative to modify or change time\n");
#if defined( HAVE_UTIMENSAT ) && defined( AT_SYMLINK_NOFOLLOW )
        printf("    --relatime-rar\t    like --relatime but also update main archive file(s)\n");
#endif
}

/* FUSE API specific keys continue where 'optdb' left off */
enum {
        OPT_KEY_HELP = OPT_KEY_END,
        OPT_KEY_VERSION,
};

static struct fuse_opt rar2fs_opts[] = {
        FUSE_OPT_KEY("-V",              OPT_KEY_VERSION),
        FUSE_OPT_KEY("--version",       OPT_KEY_VERSION),
        FUSE_OPT_KEY("-h",              OPT_KEY_HELP),
        FUSE_OPT_KEY("--help",          OPT_KEY_HELP),
        FUSE_OPT_END
};

static struct option longopts[] = {
        {"show-comp-img",     no_argument, NULL, OPT_ADDR(OPT_KEY_SHOW_COMP_IMG)},
        {"preopen-img",       no_argument, NULL, OPT_ADDR(OPT_KEY_PREOPEN_IMG)},
        {"fake-iso",    optional_argument, NULL, OPT_ADDR(OPT_KEY_FAKE_ISO)},
        {"exclude",     required_argument, NULL, OPT_ADDR(OPT_KEY_EXCLUDE)},
        {"seek-length", required_argument, NULL, OPT_ADDR(OPT_KEY_SEEK_LENGTH)},
#if defined ( HAVE_SCHED_SETAFFINITY ) && defined ( HAVE_CPU_SET_T )
        {"no-smp",            no_argument, NULL, OPT_ADDR(OPT_KEY_NO_SMP)},
#endif
        {"img-type",    required_argument, NULL, OPT_ADDR(OPT_KEY_IMG_TYPE)},
        {"no-lib-check",      no_argument, NULL, OPT_ADDR(OPT_KEY_NO_LIB_CHECK)},
#ifndef USE_STATIC_IOB_
        {"hist-size",   required_argument, NULL, OPT_ADDR(OPT_KEY_HIST_SIZE)},
        {"iob-size",    required_argument, NULL, OPT_ADDR(OPT_KEY_BUF_SIZE)},
#endif
        {"save-eof",          no_argument, NULL, OPT_ADDR(OPT_KEY_SAVE_EOF)},
        {"no-expand-cbr",     no_argument, NULL, OPT_ADDR(OPT_KEY_NO_EXPAND_CBR)},
        {"relatime",          no_argument, NULL, OPT_ADDR(OPT_KEY_ATIME)},
#if defined( HAVE_UTIMENSAT ) && defined( AT_SYMLINK_NOFOLLOW )
        {"relatime-rar",      no_argument, NULL, OPT_ADDR(OPT_KEY_ATIME_RAR)},
#endif
        {NULL,                          0, NULL, 0}
};

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2fs_opt_proc(void *data, const char *arg, int key,
                struct fuse_args *outargs)
{
        const char *const argv[2] = {outargs->argv[0], arg};
        /* Some early fuse versions require that fuse_main() is called
         * with a valid pointer to a fuse_operations struct? */
        static struct fuse_operations dummy_ops;

        (void)data;             /* touch */

        switch (key) {
        case FUSE_OPT_KEY_NONOPT:
                if (!OPT_SET(OPT_KEY_SRC)) {
                        optdb_save(OPT_KEY_SRC, arg);
                        return 0;
                }
                if (!OPT_SET(OPT_KEY_DST)) {
                        optdb_save(OPT_KEY_DST, arg);
                        return 0;
                }
                usage(outargs->argv[0]);
                return -1;

        case FUSE_OPT_KEY_OPT:
                optind=0;
                opterr=1;
                int opt = getopt_long(2, (char *const*)argv, "dfs", longopts, NULL);
                if (opt == '?')
                        return -1;
                if (opt >= OPT_ADDR(0)) {
                        if (!optdb_save(OPT_ID(opt), optarg))
                                return 0;
                        usage(outargs->argv[0]);
                        return -1;
                }
                return 1;

        case OPT_KEY_HELP:
                fprintf(stderr,
                        "usage: %s source mountpoint [options]\n"
                        "\n"
                        "general options:\n"
                        "    -o opt,[opt...]        mount options\n"
                        "    -h   --help            print help\n"
                        "    -V   --version         print version\n"
                        "\n", outargs->argv[0]);
                fuse_opt_add_arg(outargs, "-ho");
                fuse_main(outargs->argc, outargs->argv,
                                &dummy_ops, NULL);
                print_help();
                exit(0);

        case OPT_KEY_VERSION:
                print_version();
                fuse_opt_add_arg(outargs, "--version");
                fuse_main(outargs->argc, outargs->argv,
                                &dummy_ops, NULL);
                exit(0);

        default:
                break;
        }

        return 1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
int main(int argc, char *argv[])
{
        int res = 0;

#ifdef HAVE_SETLOCALE
        setlocale(LC_CTYPE, "");
#endif

#ifdef HAVE_UMASK
        umask_ = umask(0);
        umask(umask_);
#endif

#ifdef HAVE_ICONV
#if 0
        icd = iconv_open("UTF-8", "wchar_t");
        if (icd == (iconv_t)-1) {
                perror("iconv_open");
        }
#else
        icd = (iconv_t)-1;
#endif
#endif

        optdb_init();

        long ps = -1;
#if defined ( _SC_PAGE_SIZE )
        ps = sysconf(_SC_PAGE_SIZE);
#else
#if defined ( _SC_PAGESIZE )
        ps = sysconf(_SC_PAGESIZE);
#endif
#endif
        if (ps != -1)
                page_size_ = ps;
        else
                page_size_ = 4096;

        struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
        if (fuse_opt_parse(&args, NULL, rar2fs_opts, rar2fs_opt_proc))
                return -1;

        /* Check src/dst path */
        if (OPT_SET(OPT_KEY_SRC) && OPT_SET(OPT_KEY_DST)) {
                char *dst_path = NULL;
                char *src_path = NULL;
                if (check_paths(argv[0],
                                OPT_STR(OPT_KEY_SRC, 0),
                                OPT_STR(OPT_KEY_DST, 0),
                                &src_path, &dst_path, 1))
                        return -1;

                optdb_save(OPT_KEY_SRC, src_path);
                optdb_save(OPT_KEY_DST, dst_path);
                free(src_path);
                free(dst_path);
        } else {
                usage(argv[0]);
                return 0;
        }

        /* Check I/O buffer and history size */
        if (check_iob(argv[0], 1))
                return -1;

        /* Check library versions */
        if (!OPT_SET(OPT_KEY_NO_LIB_CHECK)) {
                if (check_libunrar(1) || check_libfuse(1))
                        return -1;
        }

        fuse_opt_add_arg(&args, "-osync_read,fsname=rar2fs,subtype=rar2fs");
        if (OPT_SET(OPT_KEY_DST))
                fuse_opt_add_arg(&args, OPT_STR(OPT_KEY_DST, 0));

        /*
         * Initialize logging.
         * LOG_PERROR is not in POSIX.1-2001 and if it is not defined make
         * sure we set it to something that will not result in a compilation
         * error.
         */
#ifndef LOG_PERROR
#define LOG_PERROR 0
#endif
        openlog("rar2fs", LOG_CONS | LOG_PERROR | LOG_PID, LOG_USER);

	/*
	 * All static setup is ready, the rest is taken from the configuration.
         * Continue in work() function which will not return until the process
         * is about to terminate.
         */
        res = work(&args);

        /* Clean up what has not already been taken care of */
        fuse_opt_free_args(&args);
        optdb_destroy();
        if (mount_type == MOUNT_ARCHIVE)
                dir_list_free(arch_list);
        if (fs_loop) {
                free(fs_loop_mp_root);
                free(fs_loop_mp_base);
        }

#ifdef HAVE_ICONV
        if (icd != (iconv_t)-1 && iconv_close(icd) != 0)
                perror("iconv_close");
#endif

        closelog();

        return res;
}
