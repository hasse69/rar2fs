/*
    Copyright (C) 2009-2014 Hans Beckerus (hans.beckerus#AT#gmail.com)

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
#ifdef HAVE_ICONV
#include <iconv.h>
#endif
#include <assert.h>
#include "version.h"
#include "debug.h"
#include "index.h"
#include "dllwrapper.h"
#include "filecache.h"
#include "iobuffer.h"
#include "optdb.h"
#include "sighandler.h"
#include "dirlist.h"

#define E_TO_MEM 0
#define E_TO_TMP 1

#define MOUNT_FOLDER  0
#define MOUNT_ARCHIVE 1

struct vol_handle {
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
        dir_elem_t *entry_p;
        struct vol_handle *volHdl;
        int pfd1[2];
        int pfd2[2];
        volatile int terminate;
        pthread_t thread;
        pthread_mutex_t mutex;
        /* mmap() data */
        void *mmap_addr;
        FILE *mmap_fp;
        int mmap_fd;
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
        dir_elem_t *entry_p;                    /* type = IO_TYPE_ISO */
        char *path;                             /* type = IO_TYPE_DIR */
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

#define WAIT_THREAD(pfd) \
        do {\
                int fd = pfd[0];\
                int nfsd = fd+1;\
                fd_set rd;\
                FD_ZERO(&rd);\
                FD_SET(fd, &rd);\
                int retval = select(nfsd, &rd, NULL, NULL, NULL); \
                if (retval == -1) {\
                        if (errno != EINTR) \
                                perror("select");\
                } else if (retval) {\
                        /* FD_ISSET(0, &rfds) will be true. */\
                        char buf[2];\
                        NO_UNUSED_RESULT read(fd, buf, 1); /* consume byte */\
                        printd(4, "%lu thread wakeup (%d, %u)\n",\
                               (unsigned long)pthread_self(),\
                               retval,\
                               (int)buf[0]);\
                } else {\
                        perror("select");\
                }\
        } while (0)

#define WAKE_THREAD(pfd, op) \
        do {\
                /* Wakeup the reader thread */ \
                char buf[2]; \
                buf[0] = (op); \
                if (write((pfd)[1], buf, 1) != 1) \
                        perror("write"); \
        } while (0)

#define IS_ISO(s) (!strcasecmp((s)+(strlen(s)-4), ".iso"))
#define IS_AVI(s) (!strcasecmp((s)+(strlen(s)-4), ".avi"))
#define IS_MKV(s) (!strcasecmp((s)+(strlen(s)-4), ".mkv"))
#define IS_RAR(s) (!strcasecmp((s)+(strlen(s)-4), ".rar"))
#define IS_CBR(s) (!OPT_SET(OPT_KEY_NO_EXPAND_CBR) && \
                        !strcasecmp((s)+(strlen(s)-4), ".cbr"))

/*
 * This is to the handle the workaround for the destroyed file pointer
 * position in some early libunrar5 versions when calling RARInitArchiveEx().
 */
#ifdef HAVE_FMEMOPEN
#define INIT_FP_ARG_(fp) (fp),0
#else
#define INIT_FP_ARG_(fp) (fp),1
#endif

#ifdef HAVE_ICONV
static iconv_t icd;
#endif
long page_size_ = 0;
static int mount_type;
static struct dir_entry_list arch_list_root;        /* internal list root */
static struct dir_entry_list *arch_list = &arch_list_root;
static pthread_attr_t thread_attr;
static unsigned int rar2_ticks;
static int fs_terminated = 0;
static int fs_loop = 0;
static int64_t blkdev_size = -1;
static mode_t umask_ = 0022;

#ifdef __linux
#define IS_BLKDEV() (blkdev_size >= 0)
#else
#define IS_BLKDEV() (0)
#endif
#define BLKDEV_SIZE() (blkdev_size > 0 ? blkdev_size : 0)

static int extract_rar(char *arch, const char *file, FILE *fp, void *arg);

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

static int extract_index(const dir_elem_t *entry_p, off_t offset);
static int preload_index(struct io_buf *buf, const char *path);

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
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
static wchar_t *get_password(const char *file, wchar_t *buf, size_t len)
#else
static char *get_password(const char *file, char *buf, size_t len)
#endif
{
        if (file) {
                size_t l = strlen(file);
                char *F = alloca(l + 2); /* might try . below */
                strcpy(F, file);
                strcpy(F + (l - 4), ".pwd");
                FILE *fp = fopen(F, "r");
                if (!fp) {
                        char *tmp1 = strdup(file);
                        strcpy(F, dirname(tmp1));
                        free(tmp1);
                        strcat(F, "/.");
                        tmp1 = strdup(file);
                        strcat(F, basename(tmp1));
                        free(tmp1);
                        strcpy(F + (l - 3), ".pwd");
                        fp = fopen(F, "r");
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
                        return buf;
                }
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
static dir_elem_t *path_lookup_miss(const char *path, struct stat *stbuf)
{
        struct stat st;
        char *root;

        printd(3, "MISS    %s\n", path);

        ABS_ROOT(root, path);

        /* Check if the missing file can be found on the local fs */
        if (!lstat(root, stbuf ? stbuf : &st)) {
                printd(3, "STAT retrieved for %s\n", root);
                return LOCAL_FS_ENTRY;
        }

        /* Check if the missing file is a fake .iso */
        if (OPT_SET(OPT_KEY_FAKE_ISO) && IS_ISO(root)) {
                dir_elem_t *e_p;
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
                        e_p->name_p = strdup(path);
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
static dir_elem_t *path_lookup(const char *path, struct stat *stbuf)
{
        dir_elem_t *e_p = filecache_get(path);
        if (e_p && !e_p->flags.fake_iso) {
                if (stbuf)
                        memcpy(stbuf, &e_p->stat, sizeof(struct stat));
                return e_p;
        }
        /* Do not remember fake .ISO entries between eg. getattr() calls */
        if (e_p && e_p->flags.fake_iso)
                filecache_invalidate(path);
        return path_lookup_miss(path, stbuf);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *extract_to(const char *file, off_t sz, const dir_elem_t *entry_p,
                                int oper)
{
        ENTER_("%s", file);

        int out_pipe[2] = {-1, -1};

        if (pipe(out_pipe) != 0)      /* make a pipe */
                return MAP_FAILED;

        printd(3, "Extracting %" PRIu64 "bytes resident in %s\n", sz, entry_p->rar_p);
        pid_t pid = fork();
        if (pid == 0) {
                int ret;
                close(out_pipe[0]);
                ret = extract_rar(entry_p->rar_p, file, NULL,
                                        (void *)(uintptr_t) out_pipe[1]);
                close(out_pipe[1]);
                _exit(ret);
        } else if (pid < 0) {
                close(out_pipe[0]);
                close(out_pipe[1]);
                /* The fork failed. Report failure. */
                return MAP_FAILED;
        }

        close(out_pipe[1]);

        FILE *tmp = NULL;
        char *buffer = malloc(sz);
        if (!buffer)
                return MAP_FAILED;

        if (oper == E_TO_TMP)
                tmp = tmpfile();

        off_t off = 0;
        ssize_t n;
        do {
                /* read from pipe into buffer */
                n = read(out_pipe[0], buffer + off, sz - off);
                if (n == -1) {
                        if (errno == EINTR)
                                continue;
                        perror("read");
                        break;
                }
                off += n;
        } while (n && off != sz);

        /* Sync */
        if (waitpid(pid, NULL, 0) == -1) {
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
        } 

        /* Check for incomplete buffer error */
        if (off != sz) {
                free(buffer);
                return MAP_FAILED;
        }

        printd(4, "Read %" PRIu64 "bytes from PIPE %d\n", off, out_pipe[0]);
        close(out_pipe[0]);

        if (tmp) {
                if (!fwrite(buffer, sz, 1, tmp)) {
                        fclose(tmp);
                        tmp = MAP_FAILED;
                } else {
                        fseeko(tmp, 0, SEEK_SET);
                }
                free(buffer);
                return tmp;
        }

        return buffer;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static FILE *popen_(const dir_elem_t *entry_p, pid_t *cpid, void **mmap_addr,
                FILE **mmap_fp, int *mmap_fd)
{
        char *maddr = MAP_FAILED;
        FILE *fp = NULL;
        int fd = -1;
        int pfd[2] = {-1,};

        if (entry_p->flags.mmap) {
                fd = open(entry_p->rar_p, O_RDONLY);
                if (fd == -1) {
                        perror("open");
                        goto error;
                }

                if (entry_p->flags.mmap == 2) {
#ifdef HAVE_FMEMOPEN
                        maddr = extract_to(entry_p->file_p, entry_p->msize,
                                                entry_p, E_TO_MEM);
                        if (maddr != MAP_FAILED) {
                                fp = fmemopen(maddr, entry_p->msize, "r");
                                if (fp == NULL) {
                                        perror("fmemopen");
                                        goto error;
                                }
                        }
#else
                        fp = extract_to(entry_p->file_p, entry_p->msize,
                                                entry_p, E_TO_TMP);
                        if (fp == MAP_FAILED) {
                                printd(1, "Extract to tmpfile failed\n");
                                goto error;
                        }
#endif
                } else {
#if defined ( HAVE_FMEMOPEN ) && defined ( HAVE_MMAP )
                        maddr = mmap(0, P_ALIGN_(entry_p->msize), PROT_READ,
                                                MAP_SHARED, fd, 0);
                        if (maddr != MAP_FAILED) {
                                fp = fmemopen(maddr + entry_p->offset,
                                                        entry_p->msize -
                                                        entry_p->offset, "r");
                                if (fp == NULL) {
                                        perror("fmemopen");
                                        goto error;
                                }
                        } else {
                                perror("mmap");
                                goto error;
                        }
#else
                        fp = fopen(entry_p->rar_p, "r");
                        if (fp)
                                fseeko(fp, entry_p->offset, SEEK_SET);
                        else
                                goto error;
#endif
                }

                *mmap_addr = maddr;
                *mmap_fp = fp;
                *mmap_fd = fd;
        }

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
                                  entry_p->flags.mmap
                                        ? entry_p->file2_p
                                        : entry_p->file_p,
                                  fp, (void *)(uintptr_t) pfd[1]);
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
        if (maddr != MAP_FAILED) {
#ifdef HAVE_MMAP
                if (entry_p->flags.mmap == 1)
                        munmap(maddr, P_ALIGN_(entry_p->msize));
                else
#endif
                        free(maddr);
        }
        if (fp)
                fclose(fp);
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
        pid_t wpid;
        int status = 0;

        fclose(fp);
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

/* Size of file data in first volume number */
#define VOL_REAL_SZ op->entry_p->vsize_real

/* Calculate volume number base offset using input file offset */
#define VOL_NO(off, d)\
        (off < VOL_FIRST_SZ ? 0 : ((off - VOL_FIRST_SZ) /\
                (VOL_NEXT_SZ - (d))) + 1)

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static char *get_vname(int t, const char *str, int vol, int len, int pos)
{
        ENTER_("%s   vol=%d, len=%d, pos=%d", str, vol, len, pos);
        char *s = strdup(str);
        if (!vol)
                return s;
        if (t) {
                char f[16];
                char f1[16];
                sprintf(f1, "%%0%dd", len);
                sprintf(f, f1, vol);
                strncpy(&s[pos], f, len);
        } else {
                char f[16];
                int lower = s[pos - 1] >= 'r';
                if (vol == 1) {
                        sprintf(f, "%s", (lower ? "ar" : "AR"));
                } else if (vol <= 101) {
                        sprintf(f, "%02d", (vol - 2));
                } else { /* Possible, but unlikely */
                        sprintf(f, "%c%02d", (lower ? 'r' : 'R') + (vol - 2) / 100,
                                                (vol - 2) % 100);
                        --pos;
                        ++len;
                }
                strncpy(&s[pos], f, len);
        }
        return s;
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

        while (size) {
                FILE *fp;
                off_t src_off = 0;
                struct vol_handle *vol_p = NULL;
                if (op->entry_p->flags.multipart) {
                        /* 
                         * RAR5.x (and later?) have a 1 byte volume number in 
                         * the Main Archive Header for volume 1-127 and 2 byte
                         * for the rest. Check if we need to compensate. 
                         */
                        int vol = VOL_NO(offset, 0);
                        if (op->entry_p->flags.vno_in_header && 
                                        op->entry_p->vno_base < 128 &&
                                        (vol + op->entry_p->vno_base) > 128) {
                                int vol_contrib = 128 - op->entry_p->vno_base;
                                vol = vol_contrib + VOL_NO(offset - (vol_contrib * VOL_NEXT_SZ), 1);
                                chunk = (VOL_NEXT_SZ - 1) -
                                          ((offset - (VOL_FIRST_SZ + (127 * VOL_NEXT_SZ))) %
                                          (VOL_NEXT_SZ - 1));
                        } else {
                                chunk = offset < VOL_FIRST_SZ
                                        ? VOL_FIRST_SZ - offset
                                        : (VOL_NEXT_SZ) -
                                          ((offset - VOL_FIRST_SZ) % (VOL_NEXT_SZ));
                        }

                        /* keep current open file */
                        if (vol != op->vno) {
                                /* close/open */
                                op->vno = vol;
                                if (op->volHdl) {
                                        vol_p = &op->volHdl[vol];
                                        if (vol_p->fp) {
                                                fp = vol_p->fp;
                                                src_off = VOL_REAL_SZ - chunk;
                                                if (src_off != vol_p->pos)
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
                                if (op->volHdl && op->volHdl[vol].fp)
                                        fp = op->volHdl[vol].fp;
                                else
                                        fp = op->fp;
                        }
seek_check:
                        if (force_seek || offset != op->pos) {
                                src_off = VOL_REAL_SZ - chunk;
                                printd(3, "SEEK src_off = %" PRIu64 ", "
                                                "VOL_REAL_SZ = %" PRIu64 "\n",
                                                src_off, VOL_REAL_SZ);
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
                if (vol_p)
                        vol_p->pos += n;
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
        if(!offset) {
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
static void sync_thread_read(int *pfd1, int *pfd2)
{
       do {
                errno = 0;
                WAKE_THREAD(pfd1, 1);
                WAIT_THREAD(pfd2);
        } while (errno == EINTR);
}

static void sync_thread_noread(int *pfd1, int *pfd2)
{
        do {
                errno = 0;
                WAKE_THREAD(pfd1, 2);
                WAIT_THREAD(pfd2);
        } while (errno == EINTR);
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
        fprintf(fp, "seq=%d offset: %" PRIu64 "   size: %zu   buf: %p", seq, offset, size, buf);
        size = size > 64 ? 64 : size;
        if (fp) {
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
                        dir_elem_t *e_p; /* "real" cache entry */
                        if (op->entry_p->flags.save_eof) {
                                pthread_mutex_lock(&file_access_mutex);
                                e_p = filecache_get(op->entry_p->name_p);
                                if (e_p)
                                        e_p->flags.save_eof = 0;
                                pthread_mutex_unlock(&file_access_mutex);
                                op->entry_p->flags.save_eof = 0; 
                                if (!extract_index(op->entry_p, offset)) {
                                        if (!preload_index(op->buf, op->entry_p->name_p)) {
                                                op->seq++;
                                                goto check_idx;
                                        }
                                }
                        }
                        pthread_mutex_lock(&file_access_mutex);
                        e_p = filecache_get(op->entry_p->name_p);
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
                sync_thread_read(op->pfd1, op->pfd2);
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
                                dir_elem_t *e_p; /* "real" cache entry */ 
                                printd(3, "seq=%d    long jump hack2    offset=%" PRIu64 ","
                                                " size=%zu, buf->offset=%" PRIu64 "\n",
                                                op->seq, offset, size,
                                                op->buf->offset);
                                op->seq--;      /* pretend it never happened */
                                pthread_mutex_lock(&file_access_mutex);
                                e_p = filecache_get(op->entry_p->name_p);
                                if (e_p)
                                        e_p->flags.direct_io = 1;
                                pthread_mutex_unlock(&file_access_mutex);
                                op->entry_p->flags.direct_io = 1;
                                memset(buf, 0, size);
                                n += size;
                                goto out;
                        }
                }

                pthread_mutex_lock(&op->mutex);
                if (!op->terminate) {   /* make sure thread is running */
                        pthread_mutex_unlock(&op->mutex);
                        /* Take control of reader thread */
                        sync_thread_noread(op->pfd1, op->pfd2); /* XXX really not needed due to call above */
                        while (!feof(op->fp) &&
                                        offset > op->buf->offset) {
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
                } else {
                        pthread_mutex_unlock(&op->mutex);
                }
        }

        if (size) {
                int off = offset - op->pos;
                n += readFrom(buf, op->buf, size, off);
                op->pos += (off + size);
                if (!op->terminate)
                        WAKE_THREAD(op->pfd1, 0);
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

        if (FH_TOIO(fi->fh)->type == IO_TYPE_INFO) {
                if (FH_TOBUF(fi->fh))
                        free(FH_TOBUF(fi->fh));
        } else if (FH_TOFD(fi->fh)) {
                close(FH_TOFD(fi->fh));
        }
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
        fprintf(stderr, "%10s = %" PRIu64 "\n", #m , (unsigned long long)stbuf->m)

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
        RAROpenArchiveDataEx d;
        int files = 0;

        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = strdup(arch);
        d.OpenMode = RAR_OM_LIST;

        while (1) {
                HANDLE hdl = RAROpenArchiveEx(&d);
                if (d.OpenResult)
                        break;
                if (!(d.Flags & MHD_VOLUME)) {
                        files = 1;
                        list = dir_entry_add(list, d.ArcName, NULL, DIR_E_NRM);
                        RARCloseArchive(hdl);
                        break;
                }
                if (!files && !(d.Flags & MHD_FIRSTVOLUME))
                        break;

                ++files;
                list = dir_entry_add(list, d.ArcName, NULL, DIR_E_NRM);
                RARCloseArchive(hdl);
                RARNextVolumeName(d.ArcName, !(d.Flags & MHD_NEWNUMBERING));
        }
        free(d.ArcName);
        return files;
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
static int get_vformat(const char *s, int t, int *l, int *p)
{
        int len = 0;
        int pos = 0;
        int vol = 0;
        const size_t SLEN = strlen(s);
        if (t) {
                int dot = 0;
                len = SLEN - 1;
                while (dot < 2 && len >= 0) {
                        if (s[len--] == '.')
                                ++dot;
                }
                if (len >= 0) {
                        pos = len + 1;
                        len = SLEN - pos;
                        if (len >= 10) {
                                if ((s[pos + 1] == 'p' || s[pos + 1] == 'P') &&
                                    (s[pos + 2] == 'a' || s[pos + 2] == 'A') &&
                                    (s[pos + 3] == 'r' || s[pos + 3] == 'R') &&
                                    (s[pos + 4] == 't' || s[pos + 4] == 'T')) {
                                        pos += 5;       /* - ".part" */
                                        len -= 9;       /* - ".ext" */
                                        vol = strtoul(&s[pos], NULL, 10);
                                } 
                        }
                }
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
                                        vol = 1;
                                } else {
                                        int lower = s[pos - 1] >= 'r';
                                        errno = 0;
                                        vol = strtoul(&s[pos], NULL, 10) + 2 +
                                                /* Possible, but unlikely */
                                                (100 * (s[pos - 1] - (lower ? 'r' : 'R')));
                                        vol = errno ? 0 : vol;
                                }
                        }
                }
        }
        if (l) *l = vol ? len : 0;
        if (p) *p = vol ? pos : 0;
        return vol ? vol : 1;
}

#define IS_AVI(s) (!strcasecmp((s)+(strlen(s)-4), ".avi"))
#define IS_MKV(s) (!strcasecmp((s)+(strlen(s)-4), ".mkv"))
#define IS_RAR(s) (!strcasecmp((s)+(strlen(s)-4), ".rar"))
#define IS_CBR(s) (!OPT_SET(OPT_KEY_NO_EXPAND_CBR) && \
                        !strcasecmp((s)+(strlen(s)-4), ".cbr"))
#define IS_RXX(s) (is_rxx_vol(s))
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
static int extract_index(const dir_elem_t *entry_p, off_t offset)
{
        int e = ERAR_BAD_DATA;
        struct RAROpenArchiveDataEx d;
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = entry_p->rar_p;
        d.OpenMode = RAR_OM_EXTRACT;

        struct RARHeaderData header;
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

        ABS_ROOT(r2i, entry_p->name_p);
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
                if (RARReadHeader(hdl, &header))
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
                                perror("write");
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
static int extract_rar(char *arch, const char *file, FILE *fp, void *arg)
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
        struct RARHeaderData header;
        HANDLE hdl = fp 
                ? RARInitArchiveEx(&d, INIT_FP_ARG_(fp))
                : RAROpenArchiveEx(&d);
        if (d.OpenResult)
                goto extract_error;

        header.CmtBufSize = 0;
        while (1) {
                if (RARReadHeader(hdl, &header))
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

        if (hdl) {
                if (!fp)
                        RARCloseArchive(hdl);
                else
                        RARFreeArchive(hdl);
        }

        return ret;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void set_rarstats(dir_elem_t *entry_p, RARArchiveListEx *alist_p,
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
static dir_elem_t *lookup_filecopy(const char *path, RARArchiveListEx *next,
                const char *rar_root, int display)

{
        dir_elem_t *e_p = NULL;
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
                                ABS_MP(mp2, path, basename(tmp));
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
static void resolve_filecopy(RARArchiveListEx *next, RARArchiveListEx *root)

{
        char *tmp = malloc(sizeof(next->LinkTarget));
        if (tmp) {
                if (wide_to_char(tmp, next->LinkTargetW,
                                        sizeof(next->LinkTarget)) != (size_t)-1) {
                        DOS_TO_UNIX_PATH(tmp);
                        RARArchiveListEx *next2 = root;
                        while (next2) {
                                if (!strcmp(next2->hdr.FileName, tmp)) {
                                        memcpy(&next->hdr, &next2->hdr, sizeof(struct RARHeaderDataEx));
                                        next->HeadSize = next2->HeadSize;
                                        next->Offset = next2->Offset;
                                        break;
                                }
                                next2 = next2->next;
                        }
                }
                free(tmp);
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int listrar_rar(const char *path, struct dir_entry_list **buffer,
                const char *arch, HANDLE hdl, const RARArchiveListEx *next,
                const dir_elem_t *entry_p, unsigned int mh_flags)
{
        printd(3, "%llu byte RAR file %s found in archive %s\n",
               GET_RAR_PACK_SZ(&next->hdr), entry_p->name_p, arch);

        int result = 1;
        RAROpenArchiveDataEx d2;
        memset(&d2, 0, sizeof(RAROpenArchiveDataEx));
        d2.ArcName = entry_p->name_p;
        d2.OpenMode = RAR_OM_LIST;

        HANDLE hdl2 = NULL;
        FILE *fp = NULL;
        char *maddr = MAP_FAILED;
        off_t msize = 0;
        int mflags = 0;

        if (next->hdr.Method == FHD_STORING && 
                        !(mh_flags & (MHD_PASSWORD | MHD_VOLUME))) {
                struct stat st;
                int fd = fileno(RARGetFileHandle(hdl));
                if (fd == -1)
                        goto file_error;
                (void)fstat(fd, &st);
#if defined ( HAVE_FMEMOPEN ) && defined ( HAVE_MMAP )
                if (!IS_BLKDEV() || BLKDEV_SIZE()) {
                        msize = IS_BLKDEV() ? BLKDEV_SIZE() : st.st_size; 
                        maddr = mmap(0, P_ALIGN_(msize), PROT_READ,
                                                MAP_SHARED, fd, 0);
                        if (maddr != MAP_FAILED)
                                fp = fmemopen(maddr + (next->Offset + next->HeadSize),
                                                GET_RAR_PACK_SZ(&next->hdr), "r");
                } else {
#endif
                        msize = st.st_size; 
                        fp = fopen(entry_p->rar_p, "r");
                        if (fp)
                                fseeko(fp, next->Offset + next->HeadSize,
                                                        SEEK_SET);
#if defined ( HAVE_FMEMOPEN ) && defined ( HAVE_MMAP )
                }
#endif
                mflags = 1;
        } else {
#ifdef HAVE_FMEMOPEN
                maddr = extract_to(next->hdr.FileName, GET_RAR_SZ(&next->hdr),
                                                        entry_p, E_TO_MEM);
                if (maddr != MAP_FAILED)
                        fp = fmemopen(maddr, GET_RAR_SZ(&next->hdr), "r");
#else
                fp = extract_to(next->hdr.FileName, GET_RAR_SZ(&next->hdr),
                                                        entry_p, E_TO_TMP);
                if (fp == MAP_FAILED) {
                        fp = NULL;
                        printd(1, "Extract to tmpfile failed\n");
                }
#endif
                msize = GET_RAR_SZ(&next->hdr);
                mflags = 2;
        }

        if (fp) {
#ifndef HAVE_MMAP
                hdl2 = RARInitArchiveEx(&d2, fp, 1);
#else
                hdl2 = IS_BLKDEV() && !BLKDEV_SIZE() 
                        ? RARInitArchiveEx(&d2, fp, 1)
                        : RARInitArchiveEx(&d2, INIT_FP_ARG_(fp));
#endif
        }
        if (!fp || d2.OpenResult || (d2.Flags & MHD_VOLUME))
                goto file_error;

        int dll_result;
        RARArchiveListEx LL;
        RARArchiveListEx *next2 = &LL;
        if (!RARListArchiveEx(hdl2, next2, NULL, &dll_result))
                goto file_error;

        char *tmp1 = strdup(entry_p->name_p);
        char *rar_root = dirname(tmp1);
        size_t rar_root_len = strlen(rar_root);
        size_t path_len = strlen(path);
        int is_root_path = !strcmp(rar_root, path);

        while (next2) {
                dir_elem_t *entry2_p;
                DOS_TO_UNIX_PATH(next2->hdr.FileName);

                printd(3, "File inside archive is %s\n", next2->hdr.FileName);

                /* Skip compressed image files */
                if (!OPT_SET(OPT_KEY_SHOW_COMP_IMG) &&
                                next2->hdr.Method != FHD_STORING &&
                                IS_IMG(next2->hdr.FileName)) {
                        next2 = next2->next;
                        continue;
                }

                int display = 0;
                char *rar_file;
                ABS_MP(rar_file, rar_root, next2->hdr.FileName);
                char *tmp2 = strdup(next2->hdr.FileName);
                char *rar_name = dirname(tmp2);

                if (is_root_path) {
                        if (!CHRCMP(rar_name, '.'))
                                display = 1;

                        /*
                         * Handle the rare case when the parent folder does not have
                         * its own entry in the file header. The entry needs to be
                         * faked by adding it to the cache. If the parent folder is
                         * discovered later in the header the faked entry will be
                         * invalidated and replaced with the real stats.
                         */
                        if (!display) {
                                char *safe_path = strdup(rar_name);
                                if (!strcmp(basename(safe_path), rar_name)) {
                                        char *mp;
                                        ABS_MP(mp, path, rar_name);
                                        entry2_p = filecache_get(mp);
                                        if (entry2_p == NULL) {
                                                printd(3, "Adding %s to cache\n", mp);
                                                entry2_p = filecache_alloc(mp);
                                                entry2_p->name_p = strdup(mp);
                                                entry2_p->rar_p = strdup(arch);
                                                entry2_p->file_p = strdup(next->hdr.FileName);
                                                entry2_p->file2_p = strdup(rar_name);
                                                entry2_p->flags.force_dir = 1;
                                                entry2_p->flags.mmap = mflags;
                                                entry2_p->msize = msize;
                                                set_rarstats(entry2_p, next2, 1);
                                        }
                                        if (buffer) {
                                                *buffer = dir_entry_add_hash(
                                                        *buffer, rar_name,
                                                        &entry2_p->stat, entry2_p->dir_hash,
                                                        DIR_E_RAR);
                                        }
                                }
                                free(safe_path);
                        }
                } else {
                        if (rar_root_len < path_len)
                                if (!strcmp(path + rar_root_len + 1, rar_name))
                                        display = 1;
                }

                printd(3, "Looking up %s in cache\n", rar_file);
                entry2_p = filecache_get(rar_file);
                if (entry2_p)  {
                        /*
                         * Check if this was a forced/fake entry. In that
                         * case update it with proper stats.
                         */
                        if (entry2_p->flags.force_dir) {
                                set_rarstats(entry2_p, next2, 0);
                                entry2_p->flags.force_dir = 0;
                        }
                        goto cache_hit;
                }

                /* Allocate a cache entry for this file */
                printd(3, "Adding %s to cache\n", rar_file);
                entry2_p = filecache_alloc(rar_file);
                entry2_p->name_p = strdup(rar_file);

                if (next2->LinkTargetFlags & LINK_T_FILECOPY) {
                        dir_elem_t *e_p;
                        e_p = lookup_filecopy(path, next2, rar_root, 0);
                        if (e_p) {
                                filecache_copy(e_p, entry2_p);
                                /* We are done here! */
                                goto cache_hit;
                        }
                }
                entry2_p->rar_p = strdup(arch);
                entry2_p->file_p = strdup(next->hdr.FileName); 
                entry2_p->file2_p = strdup(next2->hdr.FileName);
                entry2_p->offset = (next->Offset + next->HeadSize);
                entry2_p->flags.mmap = mflags;
                entry2_p->msize = msize;
                entry2_p->method = next2->hdr.Method;
                entry2_p->flags.multipart = 0;
                entry2_p->flags.raw = 0;        /* no raw support yet */
                entry2_p->flags.save_eof = 0;
                set_rarstats(entry2_p, next2, 0);

cache_hit:

                if (display && buffer) {
                        char *safe_path = strdup(next2->hdr.FileName);
                        *buffer = dir_entry_add_hash(
                                        *buffer, basename(safe_path),
                                        &entry2_p->stat, entry2_p->dir_hash,
                                        DIR_E_RAR);
                        free(safe_path);
                }
                free(tmp2);
                next2 = next2->next;
        }
        RARFreeListEx(&LL);
        free(tmp1);
        result = 0;

file_error:
        if (hdl2)
                RARFreeArchive(hdl2);
        if (fp)
                fclose(fp);
        if (maddr != MAP_FAILED) {
#ifdef HAVE_MMAP
                if (mflags == 1)
                        munmap(maddr, P_ALIGN_(msize));
                else
#endif
                        free(maddr);
        }
        return result;
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
static int listrar(const char *path, struct dir_entry_list **buffer,
                const char *arch)
{
        ENTER_("%s   arch=%s", path, arch);

        pthread_mutex_lock(&file_access_mutex);
        RAROpenArchiveDataEx d;
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = (char *)arch;       /* Horrible cast! But hey... it is the API! */
        d.OpenMode = RAR_OM_LIST;
        d.Callback = list_callback_noswitch;
        d.UserData = (LPARAM)arch;
        HANDLE hdl = RAROpenArchiveEx(&d);

        /* Check for fault */
        if (d.OpenResult) {
                pthread_mutex_unlock(&file_access_mutex);
                return d.OpenResult;
        }

        int n_files;
        int dll_result;
        off_t FileDataEnd;
        RARArchiveListEx L;
        RARArchiveListEx *next = &L;
        if (!(n_files = RARListArchiveEx(hdl, next, &FileDataEnd, &dll_result))) {
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
                DOS_TO_UNIX_PATH(next->hdr.FileName);

                /* Skip compressed image files */
                if (!OPT_SET(OPT_KEY_SHOW_COMP_IMG) &&
                                next->hdr.Method != FHD_STORING &&
                                IS_IMG(next->hdr.FileName)) {
                        next = next->next;
                        --n_files;
                        continue;
                }

                int display = 0;
                char *tmp2 = strdup(next->hdr.FileName);
                char *rar_name = strdup(dirname(tmp2));
                free(tmp2);
                tmp2 = rar_name;

                if (is_root_path) {
                        if (!CHRCMP(rar_name, '.'))
                                display = 1;
                        /*
                         * Handle the rare case when the parent folder does not have
                         * its own entry in the file header. The entry needs to be
                         * faked by adding it to the cache. If the parent folder is
                         * discovered later in the header the faked entry will be
                         * invalidated and replaced with the real stats.
                         */
                        if (!display) {
                                char *safe_path = strdup(rar_name);
                                if (!strcmp(basename(safe_path), rar_name)) {
                                        char *mp;
                                        ABS_MP(mp, path, rar_name);
                                        dir_elem_t *entry_p = filecache_get(mp);
                                        if (entry_p == NULL) {
                                                printd(3, "Adding %s to cache\n", mp);
                                                entry_p = filecache_alloc(mp);
                                                entry_p->name_p = strdup(mp);
                                                entry_p->rar_p = strdup(arch);
                                                entry_p->file_p = strdup(rar_name);
                                                entry_p->flags.force_dir = 1;

                                                /* Check if part of a volume */
                                                if (d.Flags & MHD_VOLUME) {
                                                        entry_p->flags.multipart = 1;
                                                        entry_p->vtype = (d.Flags & MHD_NEWNUMBERING) ? 1 : 0;
                                                        /* 
                                                         * Make sure parent folders are always searched
                                                         * from the first volume file since sub-folders
                                                         * might actually be placed elsewhere.
                                                         */
                                                        RARVolNameToFirstName(entry_p->rar_p, !entry_p->vtype);
                                                } else {
                                                        entry_p->flags.multipart = 0;
                                                }
                                                set_rarstats(entry_p, next, 1);
                                        }

                                        if (buffer) {
                                                *buffer = dir_entry_add_hash(
                                                        *buffer, rar_name,
                                                        &entry_p->stat, entry_p->dir_hash,
                                                        DIR_E_RAR);
                                        }
                                }
                                free(safe_path);
                        }
                } else if (!strcmp(path + rar_root_len + 1, rar_name)) {
                        display = 1;
                }
                free(tmp2);

                char *mp;
                if (!display) {
                        ABS_MP(mp, (*rar_root ? rar_root : "/"),
                                        next->hdr.FileName);
                } else {
                        char *rar_dir = strdup(next->hdr.FileName);
                        ABS_MP(mp, path, basename(rar_dir));
                        free(rar_dir);
                }

                if (!IS_RAR_DIR(&next->hdr) && OPT_SET(OPT_KEY_FAKE_ISO)) {
                        int l = OPT_CNT(OPT_KEY_FAKE_ISO)
                                        ? optdb_find(OPT_KEY_FAKE_ISO, mp)
                                        : optdb_find(OPT_KEY_IMG_TYPE, mp);
                        if (l)
                                strcpy(mp + (strlen(mp) - l), "iso");
                }

                printd(3, "Looking up %s in cache\n", mp);
                dir_elem_t *entry_p = filecache_get(mp);
                if (entry_p)  {
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

                entry_p->name_p = strdup(mp);
                entry_p->rar_p = strdup(arch);
                entry_p->file_p = strdup(next->hdr.FileName);

                /* Check for .rar file inside archive */
                if (!OPT_SET(OPT_KEY_FLAT_ONLY) && IS_RAR(entry_p->name_p)
                                        && !IS_RAR_DIR(&next->hdr)) {
                        /* Only process files split across multiple volumes once */
                        int inval = (d.Flags & MHD_VOLUME) && 
                                        (next->hdr.Flags & LHD_SPLIT_BEFORE);
                        if (!inval && next->LinkTargetFlags & LINK_T_FILECOPY)
                                resolve_filecopy(next, &L);
                        if (inval || !listrar_rar(path, buffer, arch, hdl, next, entry_p, d.Flags)) {
                                /* We are done with this rar file (.rar will never display!) */
                                filecache_invalidate(mp);
                                next = next->next;
                                continue;
                        }
                }

                if (next->LinkTargetFlags & LINK_T_FILECOPY) {
                        dir_elem_t *e_p; 
                        e_p = lookup_filecopy(path, next, rar_root, display);
                        if (e_p) {
                                filecache_copy(e_p, entry_p);
                                goto cache_hit;
                        }
                }

                if (next->hdr.Method == FHD_STORING && 
                                !(next->hdr.Flags & LHD_PASSWORD) &&
                                !IS_RAR_DIR(&next->hdr)) {
                        entry_p->flags.raw = 1;
                        if ((d.Flags & MHD_VOLUME) &&   /* volume ? */
                                        ((next->hdr.Flags & (LHD_SPLIT_BEFORE | LHD_SPLIT_AFTER)))) {
                                int len, pos;

                                entry_p->flags.multipart = 1;
                                entry_p->flags.image = IS_IMG(next->hdr.FileName);
                                entry_p->vtype = (d.Flags & MHD_NEWNUMBERING) ? 1 : 0;
                                entry_p->vno_base = get_vformat(arch, entry_p->vtype, &len, &pos);

                                if (len > 0) {
                                        entry_p->vlen = len;
                                        entry_p->vpos = pos;
                                        if (!IS_RAR_DIR(&next->hdr)) {
                                                entry_p->vsize_real = FileDataEnd;
                                                entry_p->vsize_first = GET_RAR_PACK_SZ(&next->hdr);
                                                entry_p->vsize_next = FileDataEnd - (
                                                                RARGetMarkHeaderSize(hdl) +
                                                                RARGetMainHeaderSize(hdl) + next->HeadSize);
                                                /* 
                                                 * Check if we might need to compensate for the 
                                                 * 1-byte RAR5.x (and later?) volume number in 
                                                 * next main archive header.
                                                 */
                                                if (next->hdr.UnpVer >= 50) {
                                                        if (entry_p->vno_base == 1 || entry_p->vno_base == 128)
                                                                entry_p->vsize_next -= 1;
                                                        entry_p->flags.vno_in_header = 1;
                                                }
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
                                entry_p->vtype = (d.Flags & MHD_NEWNUMBERING) ? 1 : 0;
                                /* 
                                 * Make sure parent folders are always searched
                                 * from the first volume file since sub-folders
                                 * might actually be placed elsewhere.
                                 */
                                RARVolNameToFirstName(entry_p->rar_p, !entry_p->vtype);
                        } else {
                                entry_p->flags.multipart = 0;
                        }
                }
                entry_p->method = next->hdr.Method;
                set_rarstats(entry_p, next, 0);

cache_hit:

                if (display && buffer) {
                        char *safe_path = strdup(entry_p->name_p);
                        *buffer = dir_entry_add_hash(
                                        *buffer, basename(safe_path),
                                        &entry_p->stat,
                                        entry_p->dir_hash,
                                        DIR_E_RAR);
                        free(safe_path);
                }
                next = next->next;
        }

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
                        !(IS_RXX(e->d_name) && e->d_type == DT_REG));
#endif
        return !IS_RAR(e->d_name) && !IS_CBR(e->d_name) && 
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
                return (IS_RAR(e->d_name) || IS_CBR(e->d_name)) && 
                                e->d_type == DT_REG;
#endif
        return IS_RAR(e->d_name) || IS_CBR(e->d_name);
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
static void syncdir_scan(const char *dir, const char *root)
{
        struct dirent **namelist;
        unsigned int f;
        int (*filter[]) (SCANDIR_ARG3) = {f1, f2}; /* f0 not needed */

        ENTER_("%s", dir);

        for (f = 0; f < (sizeof(filter) / sizeof(filter[0])); f++) {
                int vno = 0;
                int i = 0;
                int n = scandir(root, &namelist, filter[f], alphasort);
                if (n < 0) {
                        perror("scandir");
                        return;
                }
                while (i < n) {
                        if (f == 1) {
                                /* We know this is .rNN format so this should
                                 * be a bit faster than calling get_vformat().
                                 */
                                const size_t SLEN = strlen(namelist[i]->d_name);
                                if (namelist[i]->d_name[SLEN - 1] == '0' &&
                                    namelist[i]->d_name[SLEN - 2] == '0') {
                                        vno = 2;
                                } else {
                                        ++vno;
                                } 
                        } else {
                                vno = get_vformat(namelist[i]->d_name, 1, /* new style */
                                                        NULL, NULL);
                        }
                        if (!OPT_INT(OPT_KEY_SEEK_LENGTH, 0) ||
                                        vno <= OPT_INT(OPT_KEY_SEEK_LENGTH, 0)) {
                                char *arch;
                                ABS_MP(arch, root, namelist[i]->d_name);
                                (void)listrar(dir, NULL, arch);
                        }
                        free(namelist[i]);
                        ++i;
                }
                free(namelist);
        }
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
static void readdir_scan(const char *dir, const char *root,
                struct dir_entry_list **next)
{
        struct dirent **namelist;
        unsigned int f;
        int (*filter[]) (SCANDIR_ARG3) = {f0, f1, f2};

        ENTER_("%s", dir);

        for (f = 0; f < (sizeof(filter) / sizeof(filter[0])); f++) {
                int vno = 0;
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

                                /* Hide mount point in case of a fs loop */
                                if (fs_loop) {
                                        char *path;
                                        ABS_MP(path, root, tmp);
                                        if (!strcmp(path, OPT_STR2(OPT_KEY_DST, 0)))
                                                goto next_entry;
                                }
#ifdef _DIRENT_HAVE_D_TYPE
                                if (namelist[i]->d_type == DT_REG) {
#else
                                char *path;
                                struct stat st;
                                ABS_MP(path, root, tmp);
                                (void)stat(path, &st);
                                if (S_ISREG(st.st_mode)) {
#endif
                                        tmp2 = strdup(tmp);
                                        if (convert_fake_iso(tmp2))
                                                *next = dir_entry_add(*next, tmp,
                                                               NULL, DIR_E_NRM);
                                }
                                *next = dir_entry_add(*next, tmp, NULL, DIR_E_NRM);
                                if (tmp2 != NULL)
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
                                        vno = 2;
                                } else {
                                        ++vno;
                                }
                        } else {
                                vno = get_vformat(namelist[i]->d_name, 1, /* new style */
                                                        NULL, NULL);
                        }
                        if (!OPT_INT(OPT_KEY_SEEK_LENGTH, 0) ||
                                        vno <= OPT_INT(OPT_KEY_SEEK_LENGTH, 0)) {
                                char *arch;
                                ABS_MP(arch, root, namelist[i]->d_name);
                                if (listrar(dir, next, arch))
                                        *next = dir_entry_add(*next, 
                                                       namelist[i]->d_name, 
                                                       NULL, DIR_E_NRM);
                        }

next_entry:

                        free(namelist[i]);
                        ++i;
                }
                free(namelist);
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void syncdir(const char *dir)
{
        ENTER_("%s", dir);

        DIR *dp;
        char *root;
        ABS_ROOT(root, dir);

        dp = opendir(root);
        if (dp != NULL) {
                syncdir_scan(dir, root);
                (void)closedir(dp);
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void syncrar(const char *path)
{
        ENTER_("%s", path);

        int c = 0;
        int c_end = OPT_INT(OPT_KEY_SEEK_LENGTH, 0);
        struct dir_entry_list *arch_next = arch_list_root.next;
        while (arch_next) {
                (void)listrar(path, NULL, arch_next->entry.name);
                if (c_end && ++c == c_end)
                        break;
                arch_next = arch_next->next;
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_getattr(const char *path, struct stat *stbuf)
{
        ENTER_("%s", path);
        pthread_mutex_lock(&file_access_mutex);
        if (path_lookup(path, stbuf)) {
                pthread_mutex_unlock(&file_access_mutex);
                dump_stat(stbuf);
                return 0;
        }
        pthread_mutex_unlock(&file_access_mutex);

        /*
         * There was a cache miss and the file could not be found locally!
         * This is bad! To make sure the files does not really exist all
         * rar archives need to be scanned for a matching file = slow!
         */
        if (OPT_FILTER(path))
                return -ENOENT;
        char *safe_path = strdup(path);
        syncdir(dirname(safe_path));
        free(safe_path);
        pthread_mutex_lock(&file_access_mutex);
        dir_elem_t *e_p = filecache_get(path);
        if (e_p) {
                memcpy(stbuf, &e_p->stat, sizeof(struct stat));
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
        syncrar("/");

        pthread_mutex_lock(&file_access_mutex);
        dir_elem_t *e_p = filecache_get(path);
        if (e_p) {
                memcpy(stbuf, &e_p->stat, sizeof(struct stat));
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

        int do_inval_cache = 1;

        next = next->next;
        while (next) {
                /*
                 * Purge invalid entries from the cache, display the rest.
                 * Collisions are rare but might occur when a file inside
                 * a RAR archives share the same name with a file in the
                 * back-end fs. The latter will prevail in all cases.
                 */
                if (next->entry.valid) {
                        filler(buffer, next->entry.name, next->entry.st, 0);
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

        (void)path;             /* touch */
        (void)offset;           /* touch */

        assert(FH_ISSET(fi->fh) && "bad I/O handle");

        struct io_handle *io = FH_TOIO(fi->fh);
        if (io == NULL)
                return -EIO;

        struct dir_entry_list dir_list;      /* internal list root */
        struct dir_entry_list *next = &dir_list;
        dir_list_open(next);

        DIR *dp = FH_TODP(fi->fh);
        if (dp != NULL) {
                char *root;
                ABS_ROOT(root, FH_TOPATH(fi->fh));
                readdir_scan(FH_TOPATH(fi->fh), root, &next);
                goto dump_buff;
        }

        int vol = 1;

        pthread_mutex_lock(&file_access_mutex);
        dir_elem_t *entry_p = filecache_get(FH_TOPATH(fi->fh));
        if (entry_p) {
                char *tmp = strdup(entry_p->rar_p);
                int multipart = entry_p->flags.multipart;
                short vtype = entry_p->vtype;
                pthread_mutex_unlock(&file_access_mutex);
                if (multipart) {
                        int vol_end = OPT_INT(OPT_KEY_SEEK_LENGTH, 0);
                        printd(3, "Search for local directory in %s\n", tmp);
                        while (!listrar(entry_p->name_p, &next, tmp)) {
                                ++vol;
                                if (vol_end && vol_end < vol)
                                        goto fill_buff;
                                RARNextVolumeName(tmp, !vtype);
                                printd(3, "Search for local directory in %s\n", tmp);
                        }
                } else { 
                        if (tmp) {
                                printd(3, "Search for local directory in %s\n", tmp);
                                if (!listrar(entry_p->name_p, &next, tmp)) {
                                        free(tmp);
                                        goto fill_buff;
                                }
                        }
                }
                free(tmp);
        } else {
                pthread_mutex_unlock(&file_access_mutex);
        }

        if (vol == 1) {
                dir_list_free(&dir_list);
                return -ENOENT;
        }

fill_buff:

        filler(buffer, ".", NULL, 0);
        filler(buffer, "..", NULL, 0);

dump_buff:

        dir_list_close(&dir_list);
        dump_dir_list(FH_TOPATH(fi->fh), buffer, filler, &dir_list);
        dir_list_free(&dir_list);

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

        (void)path;             /* touch */
        (void)offset;           /* touch */

        struct dir_entry_list dir_list;      /* internal list root */
        struct dir_entry_list *next = &dir_list;
        dir_list_open(next);

        int c = 0;
        int c_end = OPT_INT(OPT_KEY_SEEK_LENGTH, 0);
        struct dir_entry_list *arch_next = arch_list_root.next;
        while (arch_next) {
                (void)listrar(FH_TOPATH(fi->fh), &next, arch_next->entry.name);
                if (c_end && ++c == c_end)
                        break;
                arch_next = arch_next->next;
        }

        filler(buffer, ".", NULL, 0);
        filler(buffer, "..", NULL, 0);

        dir_list_close(&dir_list);
        dump_dir_list(FH_TOPATH(fi->fh), buffer, filler, &dir_list);
        dir_list_free(&dir_list);

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

        if (FH_TODP(fi->fh))
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
        op->terminate = 0;

        printd(4, "Reader thread started, fp=%p\n", op->fp);

        int fd = op->pfd1[0];
        int nfsd = fd + 1;
        while (!op->terminate) {
                fd_set rd;
                struct timeval tv;
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                FD_ZERO(&rd);
                FD_SET(fd, &rd);
                int retval = select(nfsd, &rd, NULL, NULL, &tv);
                if (!retval) {
                        /* timeout */
                        if (fs_terminated) {
                                if (!pthread_mutex_trylock(&op->mutex)) {
                                        op->terminate = 1;
                                        pthread_mutex_unlock(&op->mutex);
                                }
                        }
                        continue;
                }
                if (retval == -1) {
                        perror("select");
                        continue;
                }
                /* FD_ISSET(0, &rfds) will be true. */
                printd(4, "Reader thread wakeup, select()=%d\n", retval);
                char buf[2];
                NO_UNUSED_RESULT read(fd, buf, 1);      /* consume byte */
                if (buf[0] < 2 /*&& !feof(op->fp)*/)
                        (void)readTo(op->buf, op->fp, IOB_SAVE_HIST);
                if (buf[0]) {
                        printd(4, "Reader thread acknowledge\n");
                        int fd = op->pfd2[1];
                        if (write(fd, buf, 1) != 1)
                                perror("write");
                }
#if 0
                /* Early termination */
                if (feof(op->fp)) {
                        if (!pthread_mutex_trylock(&op->mutex)) {
                                op->terminate = 1;
                                pthread_mutex_unlock(&op->mutex);
                        }
                }
#endif
        }
        printd(4, "Reader thread stopped\n");
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
static int extract_rar_file_info(dir_elem_t *entry_p, struct RARWcb *wcb)
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
        if (d.OpenResult)
                return 0;

        FILE *fp = NULL;
        char *maddr = MAP_FAILED;
        HANDLE hdl2 = NULL;

        if (entry_p->flags.mmap) {
                if (entry_p->flags.mmap == 1) {
                        int fd = fileno(RARGetFileHandle(hdl));
                        if (fd == -1)
                                goto file_error;
#if defined ( HAVE_FMEMOPEN ) && defined ( HAVE_MMAP )
                        maddr = mmap(0, P_ALIGN_(entry_p->msize), PROT_READ, 
                                                MAP_SHARED, fd, 0);
                        if (maddr != MAP_FAILED)
                                fp = fmemopen(maddr + entry_p->offset, 
                                        (entry_p->msize - entry_p->offset), "r");
#else
                        fp = fopen(entry_p->rar_p, "r");
                        if (fp)
                                fseeko(fp, entry_p->offset, SEEK_SET);
#endif
                } else {
#ifdef HAVE_FMEMOPEN
                        maddr = extract_to(entry_p->file_p, entry_p->msize, 
                                                entry_p, E_TO_MEM);
                        if (maddr != MAP_FAILED)
                                fp = fmemopen(maddr, entry_p->msize, "r");
#else
                        fp = extract_to(entry_p->file_p, entry_p->msize, 
                                                entry_p, E_TO_TMP);
                        if (fp == MAP_FAILED) {
                                fp = NULL;
                                printd(1, "Extract to tmpfile failed\n");
                        }
#endif
                }
                if (fp) {
                        RAROpenArchiveDataEx d2;
                        memset(&d2, 0, sizeof(RAROpenArchiveDataEx));
                        d2.ArcName = NULL;
                        d2.OpenMode = RAR_OM_LIST;
                        d2.Callback = list_callback;
                        d2.UserData = (LPARAM)entry_p->rar_p;

                        hdl2 = RARInitArchiveEx(&d2, INIT_FP_ARG_(fp));
                        if (d2.OpenResult || (d2.Flags & MHD_VOLUME))
                                goto file_error;
                        RARGetFileInfo(hdl2, entry_p->file2_p, wcb);
                }
        } else {
                RARGetFileInfo(hdl, entry_p->file_p, wcb);
        }

file_error:

        if (fp)
                fclose(fp);
        if (maddr != MAP_FAILED) {
#ifdef HAVE_MMAP
                if (entry_p->flags.mmap == 1)
                        munmap(maddr, P_ALIGN_(entry_p->msize));
                else
#endif
                        free(maddr);
        } 
        RARCloseArchive(hdl);
        if (hdl2)
                RARFreeArchive(hdl2);
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
        dir_elem_t *entry_p;
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

                dir_elem_t *e_p = filecache_clone(entry_p);
                pthread_mutex_unlock(&file_access_mutex);
                struct RARWcb *wcb = malloc(sizeof(struct RARWcb)); 
                memset(wcb, 0, sizeof(struct RARWcb));
                FH_SETIO(fi->fh, io);
                FH_SETTYPE(fi->fh, IO_TYPE_INFO);
                FH_SETBUF(fi->fh, wcb);
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
                dir_elem_t *e_p = filecache_clone(entry_p);
                pthread_mutex_unlock(&file_access_mutex);
                if (e_p) {
                        ABS_ROOT(root, e_p->file_p);
                        res = lopen(root, fi);
                        if (res == 0) {
                                /* Override defaults */
                                FH_SETTYPE(fi->fh, IO_TYPE_ISO);
                                FH_SETENTRY(fi->fh, e_p);
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
                                                entry_p->vno_max =
                                                    pow_(10, entry_p->vlen) - 1;
                                        } else {
                                                 /* 
                                                  * Old numbering is more than obscure when
                                                  * it comes to maximum value. Lets assume 
                                                  * something high (almost realistic) here.
                                                  * Will probably hit the open file limit 
                                                  * anyway.
                                                  */
                                                 entry_p->vno_max = 901;  /* .rar -> .z99 */
                                        }
                                        op->volHdl = malloc(entry_p->vno_max * sizeof(struct vol_handle));
                                        if (op->volHdl) {
                                                memset(op->volHdl, 0, entry_p->vno_max * sizeof(struct vol_handle));
                                                char *tmp = strdup(entry_p->rar_p);
                                                int j = 0;
                                                while (j < entry_p->vno_max) {
                                                        FILE *fp_ = fopen(tmp, "r");
                                                        if (fp_ == NULL)
                                                                break;
                                                        printd(3, "pre-open %s\n", tmp);
                                                        op->volHdl[j].fp = fp_;
                                                        /* 
                                                         * The file position is only a qualified
                                                         * guess. If it is wrong it will be adjusted
                                                         * later.
                                                         */
                                                        op->volHdl[j].pos = VOL_REAL_SZ - 
                                                                        (j ? VOL_NEXT_SZ : VOL_FIRST_SZ);
                                                        printd(3, "SEEK src_off = %" PRIu64 "\n", op->volHdl[j].pos);
                                                        fseeko(fp_, op->volHdl[j].pos, SEEK_SET);
                                                        RARNextVolumeName(tmp, !entry_p->vtype);
                                                        ++j;
                                                }
                                                free(tmp);
                                        } else {
                                                printd(1, "Failed to allocate resource (%u)\n", __LINE__);
                                        }
                                } else {
                                        op->volHdl = NULL;
                                }

                                /* 
                                 * Disable flushing the kernel cache of the file contents on 
                                 * every open(). This should only be enabled on files, where 
                                 * the file data is never changed externally (not through the
                                 * mounted FUSE filesystem).
                                 * Since the file contents will never change this should save 
                                 * us from some user space calls!
                                 */
                                fi->keep_cache = 1;

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

                void *mmap_addr = NULL;
                FILE *mmap_fp = NULL;
                int mmap_fd = 0;

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
                fp = popen_(entry_p, &pid, &mmap_addr, &mmap_fp, &mmap_fd);
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

                        /*
                         * Create pipes to be used between threads.
                         * Both these pipes are used for communication between
                         * parent (this thread) and reader thread. One pipe is for
                         * requests (w->r) and the other is for responses (r<-w).
                         */
                        op->pfd1[0] = -1;
                        op->pfd1[1] = -1;
                        op->pfd2[0] = -1;
                        op->pfd2[1] = -1;
                        if (pipe(op->pfd1) == -1) {
                                perror("pipe");
                                goto open_error;
                        }
                        if (pipe(op->pfd2) == -1) {
                                perror("pipe");
                                goto open_error;
                        }

                        pthread_mutex_init(&op->mutex, NULL);

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
                        op->terminate = 1;
                        pthread_create(&op->thread, &thread_attr, reader_task, (void *)op);
                        while (op->terminate);
                        WAKE_THREAD(op->pfd1, 0);

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
                        op->mmap_addr = mmap_addr;
                        op->mmap_fp = mmap_fp;
                        op->mmap_fd = mmap_fd;

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

                goto open_error;
        }

open_error:
        pthread_mutex_unlock(&file_access_mutex);
        if (fp)
                pclose_(fp, pid);
        if (op) {
                if (op->pfd1[0] >= 0)
                        close(op->pfd1[0]);
                if (op->pfd1[1] >= 0)
                        close(op->pfd1[1]);
                if (op->pfd2[0] >= 0)
                        close(op->pfd2[0]);
                if (op->pfd2[1] >= 0)
                        close(op->pfd2[1]);
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
        pthread_mutex_unlock(&file_access_mutex);
        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline int access_chk(const char *path, int new_file)
{
        void *e;

        /*
         * To return a more correct fault code if an attempt is
         * made to create/remove a file in a RAR folder, a cache lookup
         * will tell if operation should be permitted or not.
         * Simply, if the file/folder is in the cache, forget it!
         *   This works fine in most cases but it does not work for some
         * specific programs like 'touch'. A 'touch' may result in a
         * getattr() callback even if -EPERM is returned by open() which
         * will eventually render a "No such file or directory" type of
         * error/message.
         */
        if (new_file) {
                char *p = strdup(path); /* In case p is destroyed by dirname() */
                e = (void *)filecache_get(dirname(p));
                free(p);
        } else {
                e = (void *)filecache_get(path);
        }
        return e ? 1 : 0;
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
        iobuffer_init();
        sighandler_init();

#ifdef HAVE_FMEMOPEN
        /* Check fmemopen() support */
        {
                char tmp[64];
                glibc_test = 1;
                fclose(fmemopen(tmp, 64, "r"));
                glibc_test = 0;
        }
#endif

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

        dir_elem_t *entry_p;

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
                if (op->fp) {
                        if (op->entry_p->flags.raw) {
                                if (op->volHdl) {
                                        int j;
                                        for (j = 0; j < op->entry_p->vno_max; j++) {
                                                if (op->volHdl[j].fp)
                                                        fclose(op->volHdl[j].fp);
                                        }
                                        free(op->volHdl);
                                }
                                fclose(op->fp);
                                printd(3, "Closing file handle %p\n", op->fp);
                        } else {
                                if (!op->terminate) {
                                        op->terminate = 1;
                                        WAKE_THREAD(op->pfd1, 0);
                                }
                                pthread_join(op->thread, NULL);
                                if (pclose_(op->fp, op->pid)) {
                                        printd(4, "child closed abnormaly");
                                }
                                printd(4, "PIPE %p closed towards child %05d\n",
                                               op->fp, op->pid);

                                close(op->pfd1[0]);
                                close(op->pfd1[1]);
                                close(op->pfd2[0]);
                                close(op->pfd2[1]);

#ifdef DEBUG_READ
                                fclose(op->dbg_fp);
#endif
                                if (op->entry_p->flags.mmap) {
                                        fclose(op->mmap_fp);
                                        if (op->mmap_addr != MAP_FAILED) {
#ifdef HAVE_MMAP
                                                if (op->entry_p->flags.mmap == 1)
                                                        munmap(op->mmap_addr, P_ALIGN_(op->entry_p->msize));
                                                else
#endif
                                                        free(op->mmap_addr);
                                        }
                                        close(op->mmap_fd);
                                }
                                pthread_mutex_destroy(&op->mutex);
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
                if (!rename(oldroot, newroot))
                        return 0;
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
                if (!unlink(root))
                        return 0;
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
                if (!rmdir(root))
                        return 0;
                return -errno;
        }
        return -EPERM;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int rar2_utimens(const char *path, const struct timespec ts[2])
{
        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
                int res;
                struct timeval tv[2];
                char *root;
                ABS_ROOT(root, path);

                tv[0].tv_sec = ts[0].tv_sec;
                tv[0].tv_usec = ts[0].tv_nsec / 1000;
                tv[1].tv_sec = ts[1].tv_sec;
                tv[1].tv_usec = ts[1].tv_nsec / 1000;

                res = utimes(root, tv);
                if (res == -1)
                        return -errno;
                return 0;
        }
        return -EPERM;
}

#ifdef HAVE_SETXATTR

static const char *xattr[4] = {
        "user.rar2fs.cache_method", 
        "user.rar2fs.cache_flags", 
        "user.rar2fs.cache_dir_hash", 
        NULL
};
#define XATTR_CACHE_METHOD 0
#define XATTR_CACHE_FLAGS 1
#define XATTR_CACHE_DIR_HASH 2

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
        dir_elem_t *e_p;
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
        } else if (!strcmp(name, xattr[XATTR_CACHE_DIR_HASH])) { 
                len = sizeof(uint32_t);
                xattr_no = XATTR_CACHE_DIR_HASH;
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
                case XATTR_CACHE_DIR_HASH:
                        *(uint32_t*)value = htonl(e_p->dir_hash);
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
        dir_elem_t *e_p;

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
                        printf("%s: invalid source and/or mount point\n",
                                                prog);
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
        if (!S_ISDIR(st.st_mode) ||
                        (mount_type == MOUNT_ARCHIVE &&
                        !collect_files(a1, arch_list))) {
                if (verbose)
                        printf("%s: invalid source and/or mount point\n",
                                                prog);
                return -1;
        }
        /* Do not try to use 'a1' after this call since dirname() will destroy it! */
        *src_path_out = mount_type == MOUNT_FOLDER
                ? strdup(a1) : strdup(dirname(a1));
        *dst_path_out = strdup(a2);

        /* Detect a possible file system loop */
        if (mount_type == MOUNT_FOLDER) {
                if (!strncmp(*src_path_out, *dst_path_out,
                                        strlen(*src_path_out))) {
                        if ((*dst_path_out)[strlen(*src_path_out)] == '/')
                                fs_loop = 1;
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
        .utimens = rar2_utimens,
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
#if FUSE_MAJOR_VERSION > 2 || (FUSE_MAJOR_VERSION == 2 && FUSE_MINOR_VERSION == 9)
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
        pthread_exit(NULL);
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
#ifdef SVNREV_
        snprintf(src_rev, 16, "-svnr%d", SVNREV_);
#else
#ifdef GITREV_
        snprintf(src_rev, 16, "-git%x", GITREV_);
#else
        src_rev[0] = '\0';
#endif
#endif
        printf("rar2fs v%u.%u.%u%s (DLL version %d)    Copyright (C) 2009-2014 Hans Beckerus\n",
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
        printf("    --flat-only\t\t    only expand first level of nested RAR archives\n");
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
        /* 
         * --seek-depth=n is obsolete and replaced by --flat-only
         * Provided here only for backwards compatibility. 
         */
        {"seek-depth",  required_argument, NULL, OPT_ADDR(OPT_KEY_SEEK_DEPTH)},
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
        {"flat-only",         no_argument, NULL, OPT_ADDR(OPT_KEY_FLAT_ONLY)},
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

        fuse_opt_add_arg(&args, "-s");
        fuse_opt_add_arg(&args, "-osync_read,fsname=rar2fs,subtype=rar2fs,default_permissions");
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

#ifdef HAVE_ICONV
        if (icd != (iconv_t)-1 && iconv_close(icd) != 0)
                perror("iconv_close");
#endif

        closelog();

        return res;
}
