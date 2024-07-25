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
#include "rarconfig.h"
#include "common.h"
#include "dirname.h"

#define MOUNT_FOLDER  0
#define MOUNT_ARCHIVE 1

#define RD_IDLE 0
#define RD_TERM 1
#define RD_SYNC_NOREAD 2
#define RD_SYNC_READ 3
#define RD_ASYNC_READ 4

/*#define DEBUG_READ*/

struct volume_handle {
        FILE *fp;
        off_t pos;
};

struct io_context {
        FILE* fp;
        off_t pos;
        struct iob *buf;
        pid_t pid;
        unsigned int seq;
        short vno;
        short vno_max;
        struct filecache_entry *entry_p;
        pthread_t thread;
        pthread_mutex_t raw_read_mutex;
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
#define IO_TYPE_INFO 3
#define IO_TYPE_DIR 4
        union {
                struct io_context *context;     /* type = IO_TYPE_RAR/IO_TYPE_RAW */
                int fd;                         /* type = IO_TYPE_NRM */
                DIR *dp;                        /* type = IO_TYPE_DIR */
                void *buf_p;                    /* type = IO_TYPE_INFO */
                uintptr_t bits;
        } u;
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

static long page_size_ = 0;
static int mount_type;
static struct dir_entry_list arch_list_root;        /* internal list root */
static struct dir_entry_list *arch_list = &arch_list_root;
static pthread_attr_t thread_attr;
static unsigned int rar2_ticks;
static int fs_terminated = 0;
static int warmup_cancelled = 0;
static int fs_loop = 0;
static char *fs_loop_mp_root = NULL;
static char *fs_loop_mp_base = NULL;
static size_t fs_loop_mp_base_len = 0;
static struct stat fs_loop_mp_stat;
static int64_t blkdev_size = -1;
static mode_t umask_ = 0022;
static volatile int warmup_threads = 0;
static pthread_mutex_t warmup_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t warmup_cond = PTHREAD_COND_INITIALIZER;
static char *src_path_full = NULL;

#define P_ALIGN_(a) (((a)+page_size_)&~(page_size_-1))

static int extract_rar(char *arch, const char *file, void *arg);
static int get_vformat(const char *s, int t, int *l, int *p);
static int CALLBACK list_callback_noswitch(UINT, LPARAM UserData, LPARAM, LPARAM);
static int CALLBACK list_callback(UINT, LPARAM UserData, LPARAM, LPARAM);
static void *warmup_task(void *data);

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
        int dry_run;
};

static int extract_index(const char *, const struct filecache_entry *, off_t);
static int preload_index(struct iob *, const char *);

#if RARVER_MAJOR > 4
static const char *file_cmd[] = {
        "#info",
        NULL
};
#endif

/* Strict mount options (-o) */
struct rar2fs_mount_opts {
     char *locale;
     int warmup;
};

#define RAR2FS_MOUNT_OPT(t, p, v) \
        { t, offsetof(struct rar2fs_mount_opts, p), v }
static struct rar2fs_mount_opts rar2fs_mount_opts;

#define IS_UNIX_MODE_(l) \
        ((l)->UnpVer >= 50 \
                ? (l)->HostOS == HOST_UNIX \
                : (l)->HostOS == HOST_UNIX || (l)->HostOS == HOST_BEOS)
#define IS_RAR_DIR(l) \
        ((l)->UnpVer >= 20 \
                ? ((l)->Flags&RHDF_DIRECTORY) \
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
 *****************************************************************************
 *
 ****************************************************************************/
static int get_save_eof(char *rar)
{
        if (rar) {
                char *s = OPT_STR(OPT_KEY_SRC, 0);
                int save_eof;

                if (str_beginwith(rar, s))
                        rar += strlen(s);
                save_eof = rarconfig_getprop(int, rar, RAR_SAVE_EOF_PROP);
                if (save_eof >= 0)
                        return save_eof;
                save_eof = rarconfig_getprop(int, basename(rar),
                                        RAR_SAVE_EOF_PROP);
                if (save_eof >= 0)
                        return save_eof;
        }
        return OPT_SET(OPT_KEY_SAVE_EOF) ? 1 : 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static const char *get_alias(const char *rar, const char *file)
{
        if (rar) {
                char *file_abs;
                char *rar_safe;
                char *s = OPT_STR(OPT_KEY_SRC, 0);
                const char *alias;

                ABS_MP(file_abs, "/", file);

                if (str_beginwith(rar, s))
                        rar += strlen(s);
                alias = rarconfig_getalias(rar, file_abs);
                if (alias)
                        return alias + 1;
                rar_safe = strdup(rar);
                alias = rarconfig_getalias(basename(rar_safe), file_abs);
                free(rar_safe);
                if (alias)
                        return alias + 1;
        }
        return file;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int get_seek_length(char *rar)
{
        if (rar) {
                char *s = OPT_STR(OPT_KEY_SRC, 0);
                int seek_len;

                if (str_beginwith(rar, s))
                        rar += strlen(s);
                seek_len = rarconfig_getprop(int, rar, RAR_SEEK_LENGTH_PROP);
                if (seek_len >= 0)
                        return seek_len;
                seek_len = rarconfig_getprop(int, basename(rar),
                                        RAR_SEEK_LENGTH_PROP);
                if (seek_len >= 0)
                        return seek_len;
        }
        return OPT_INT(OPT_KEY_SEEK_LENGTH, 0);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __dircache_invalidate_for_file(const char *path)
{
        char *safe_path = strdup(path);
        pthread_rwlock_wrlock(&dir_access_lock);
        dircache_invalidate(__gnu_dirname(safe_path));
        pthread_rwlock_unlock(&dir_access_lock);
        free(safe_path);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __dircache_invalidate(const char *path)
{
        pthread_rwlock_wrlock(&dir_access_lock);
        dircache_invalidate(path);
        pthread_rwlock_unlock(&dir_access_lock);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
#ifdef HAVE_VISIBILITY_ATTRIB
__attribute__((visibility("hidden")))
#endif
void __handle_sigusr1()
{
        pthread_t t;

        if (mount_type == MOUNT_FOLDER && rar2fs_mount_opts.warmup > 0) {
                pthread_mutex_lock(&warmup_lock);
                warmup_cancelled = 1;
                while (warmup_threads)
                        pthread_cond_wait(&warmup_cond, &warmup_lock);
                pthread_mutex_unlock(&warmup_lock);
                warmup_cancelled = 0;
        }
        printd(3, "Invalidating path cache\n");
        pthread_rwlock_wrlock(&file_access_lock);
        filecache_invalidate(NULL);
        pthread_rwlock_unlock(&file_access_lock);
        __dircache_invalidate(NULL);
        if (mount_type == MOUNT_FOLDER && rar2fs_mount_opts.warmup > 0)
                pthread_create(&t, NULL, warmup_task, NULL);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
#ifdef HAVE_VISIBILITY_ATTRIB
__attribute__((visibility("hidden")))
#endif
void __handle_sighup()
{
        rarconfig_destroy();
        rarconfig_init(OPT_STR(OPT_KEY_SRC, 0),
                       OPT_STR(OPT_KEY_CONFIG, 0));
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
        if (req != RD_ASYNC_READ) {
                while (op->rd_req) /* sync */
                        pthread_cond_wait(&op->rd_req_cond, &op->rd_req_mutex);
        }
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


#define IS_AVI(s) (!strcasecmp((s)+(strlen(s)-4), ".avi"))
#define IS_MKV(s) (!strcasecmp((s)+(strlen(s)-4), ".mkv"))
#define IS_RAR(s) (!strcasecmp((s)+(strlen(s)-4), ".rar"))
#define IS_CBR(s) (!OPT_SET(OPT_KEY_NO_EXPAND_CBR) && \
                        !strcasecmp((s)+(strlen(s)-4), ".cbr"))
#define IS_RXX(s) (is_rxx_vol(s))
#define IS_NNN(s) (is_nnn_vol(s))

#define VTYPE(flags) \
        ((flags & ROADF_NEWNUMBERING) ? 1 : 0)

/*!
 *****************************************************************************
 *
 ****************************************************************************/
#if RARVER_MAJOR > 4 || ( RARVER_MAJOR == 4 && RARVER_MINOR >= 20 )
#define prop_type_ wchar
#define prop_alloc_type_ wchar_t
#define prop_memcpy_ wmemcpy
static wchar_t *get_password(const char *file, wchar_t *buf, size_t len)
#else
#define prop_type_ char
#define prop_alloc_type_ char
#define prop_memcpy_ memcpy
static char *get_password(const char *file, char *buf, size_t len)
#endif
{
        char *f[2] = {NULL, NULL};
        int l[2] = {0, 0};
        int i;
        int lx;
        char *rar;
        char *tmp;
        char *s;
        const prop_alloc_type_ *password;

        if (!file)
                return NULL;

        rar = strdup(file);
        tmp = rar;
        s = OPT_STR(OPT_KEY_SRC, 0);

        int slen = strlen(s);
        if (!strncmp(rar, s, slen)){
                rar += strlen(s);
        }

        password = rarconfig_getprop(prop_type_, rar,
                                RAR_PASSWORD_PROP);
        if (password) {
                prop_memcpy_(buf, password, len);
                free(tmp);
                return buf;
        }
        password = rarconfig_getprop(prop_type_, basename(rar),
                                RAR_PASSWORD_PROP);
        if (password) {
                prop_memcpy_(buf, password, len);
                free(tmp);
                return buf;
        }
        free(tmp);

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
                                sprintf(F, "%s%s%s", __gnu_dirname(tmp1),
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

        return NULL;
}

#undef prop_type_
#undef prop_alloc_type_
#undef prop_memcpy_

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
        if (mount_type != MOUNT_ARCHIVE || !strcmp(path, "/")) {
                if (!lstat(root, stbuf ? stbuf : &st)) {
                        printd(3, "STAT retrieved for %s\n", root);
                        return LOCAL_FS_ENTRY;
                }
        }

        return NULL;
}

/*!
 *****************************************************************************
 * This function must always be called with an aquired rdlock but never
 * a wrlock. It is however possible that the rdlock is promoted to a wrlock.
 ****************************************************************************/
static struct filecache_entry *path_lookup(const char *path, struct stat *stbuf)
{
        struct filecache_entry *e_p = filecache_get(path);
        struct filecache_entry *e2_p = e_p;
        if (e_p && !e_p->flags.unresolved) {
                if (stbuf)
                        memcpy(stbuf, &e_p->stat, sizeof(struct stat));
                return e_p;
        }
        e_p = path_lookup_miss(path, stbuf);
        if (!e_p) {
                if (e2_p && e2_p->flags.unresolved) {
                        pthread_rwlock_unlock(&file_access_lock);
                        pthread_rwlock_wrlock(&file_access_lock);
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
static FILE *popen_(struct filecache_entry *entry_p, pid_t *cpid)
{
        int fd = -1;
        int pfd[2] = {-1,};

        pid_t pid;
        int ret;

        /* For folder mounts we need to perform an additional dummy
         * extraction attempt to avoid feeding the file descriptor
         * with garbage data in case of wrong password or CRC errors. */
        if (!entry_p->flags.dry_run_done && mount_type == MOUNT_FOLDER) {
                ret = extract_rar(entry_p->rar_p, entry_p->file_p, NULL);
                if (ret && ret != ERAR_UNKNOWN)
                        goto error;
                entry_p->flags.dry_run_done = 1;
        }

        if (pipe(pfd) == -1) {
                perror("pipe");
                goto error;
        }

        pid = fork();
        if (pid == 0) {
                setpgid(getpid(), 0);
                close(pfd[0]);  /* Close unused read end */
                ret = extract_rar(entry_p->rar_p, entry_p->file_p,
                                  (void *)(uintptr_t)pfd[1]);
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
#define VOL_FIRST_SZ (op->entry_p->vsize_first)

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
#define VOL_NEXT_SZ (op->entry_p->vsize_next)

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
                pos = c - s + 1;
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

        /* Sanity check */
        if (len <= 0 || pos < 0 || (size_t)(len + pos) > SLEN)
                return -1;

        if (l) *l = len;
        if (p) *p = pos;
        return vol;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __RARVolNameToFirstName(char *arch, int vtype)
{
        HANDLE h = NULL;
        RAROpenArchiveDataEx d;
        struct RARHeaderDataEx header;
        int len;
        int pos;
        int vol;
        char *s_orig = strdup(arch);
        int ret = 0;

        vol = get_vformat(arch, !vtype, &len, &pos);
        if (vol == -1)
                return -1;
        RARVolNameToFirstName(arch, vtype);

        memset(&header, 0, sizeof(header));
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.OpenMode = RAR_OM_LIST_INCSPLIT;
        d.Callback = list_callback_noswitch;

        /* Check for fault */
        if (access(arch, F_OK)) {
                if (IS_RAR(arch) && vtype) {
                        ret = -1;
                        goto out;
                }
                arch = strcpy(arch, s_orig);
        } else {
                vol = 1;
        }

        for (;;) {
                d.ArcName = (char *)arch;   /* Horrible cast! But hey... it is the API! */
                d.UserData = (LPARAM)arch;
                h = RAROpenArchiveEx(&d);
                if (d.OpenResult) {
                        ret = -1;
                        goto out;
                }
/* All pre 5.x.x versions seems to suffer from the same bug which means
 * the first volume file flag is not set properly for .rNN volumes. */
#if RARVER_MAJOR < 5
                if (IS_RAR(arch) && vtype)
                        d.Flags |= ROADF_FIRSTVOLUME;
#endif
                if (d.Flags & ROADF_FIRSTVOLUME)
                        break;

                if (vol == 0) {
                        ret = -1;
                        goto out;
                }
                --vol;
                int z;
                int i;
                for (z = 1, i = len - 1; i >= 0; i--, z *= 10)
                        arch[pos + i] = 48 + ((vol / z) % 10);

                RARCloseArchive(h);
        }

        if (RARReadHeaderEx(h, &header))
                ret = -1;

out:
        free(s_orig);
        if (h)
                RARCloseArchive(h);
        return ret;
}

#if _POSIX_TIMERS < 1
/* Some platforms, like OSX Sierra, does not seem to follow POSIX.1-2001
 * properly and fails to define _POSIX_TIMERS to a value greater than 0.
 * Double check if CLOCK_REALTIME is already defined and in that case
 * assume the POSIX timer functions are available. */
#ifndef CLOCK_REALTIME
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
        e_p = filecache_get(__gnu_dirname(tmp1));
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
                char *tmp2 = strdup(tmp1);
                int res = utimensat(0, __gnu_dirname(tmp2), tp,
                                    AT_SYMLINK_NOFOLLOW);
                if (!res && OPT_SET(OPT_KEY_ATIME_RAR)) {
                        for (;;) {
                                res = utimensat(0, tmp1, tp,
                                                AT_SYMLINK_NOFOLLOW);
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
        pthread_rwlock_wrlock(&file_access_lock);
        e_p = filecache_get(path);
        if (e_p) {
                if (e_p->stat.st_atime <= e_p->stat.st_ctime &&
                    e_p->stat.st_atime <= e_p->stat.st_mtime &&
                    (tp.tv_sec - e_p->stat.st_atime) > 86400) { /* 24h */
                        update_atime(path, e_p, &tp);
                        entry_p->stat = e_p->stat;
                }
        }
        pthread_rwlock_unlock(&file_access_lock);

no_check_atime:
        entry_p->flags.check_atime = 0;
        return;
}

/*!
 ****************************************************************************
 *
 ****************************************************************************/
static void __get_vol_and_chunk_raw(struct io_context *op, off_t offset,
                        int *vol, size_t *chunk)
{
        /*
         * RAR5 (and later?) have a one byte volume number in the
         * Main Archive Header for volume 1-127 and two bytes for the rest.
         * Check if we need to compensate.
         */
        if (op->entry_p->flags.vsize_fixup_needed) {
                int vol_contrib = 128 - op->entry_p->vno_base +
                                op->entry_p->vno_first - 1;
                off_t offset_fixup = VOL_FIRST_SZ +
                                (vol_contrib * VOL_NEXT_SZ);
                if (offset >= offset_fixup) {
                        off_t offset_left = offset - offset_fixup;
                        *vol = 1 + vol_contrib +
                                (offset_left / (VOL_NEXT_SZ - 1));
                        *chunk = (VOL_NEXT_SZ - 1) -
                                (offset_left % (VOL_NEXT_SZ - 1));
                        return;
                }
        }

        *vol = offset < VOL_FIRST_SZ ? 0 :
                1 + ((offset - VOL_FIRST_SZ) / VOL_NEXT_SZ);
        *chunk = offset < VOL_FIRST_SZ ? VOL_FIRST_SZ - offset :
                VOL_NEXT_SZ - ((offset - VOL_FIRST_SZ) % VOL_NEXT_SZ);
}

/*!
 ****************************************************************************
 *
 ****************************************************************************/
static int lread_raw(char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi)
{
        FILE *fp = NULL;
        ssize_t n = 0;
        struct io_context *op = FH_TOCONTEXT(fi->fh);
        size_t chunk;
        int tot = 0;
        int force_seek = 0;

        pthread_mutex_lock(&op->raw_read_mutex);

        op->seq++;

        printd(3, "PID %05d calling %s(), seq = %d, offset=%" PRIu64 "\n",
               getpid(), __func__, op->seq, offset);

        /*
         * Handle the case when a user tries to read outside file size.
         * This is especially important to handle here since last file in a
         * volume usually is of much less size than the others and conseqently
         * the chunk based calculation will not detect this.
         */
        if ((off_t)(offset + size) >= op->entry_p->stat.st_size) {
                if (offset > op->entry_p->stat.st_size) {
                        pthread_mutex_unlock(&op->raw_read_mutex);
                        return 0;       /* EOF */
                }
                size = op->entry_p->stat.st_size - offset;
        }

        if (op->entry_p->flags.check_atime)
                check_atime(FH_TOPATH(fi->fh), op->entry_p);

        if (!op->entry_p->flags.vsize_resolved) {
                pthread_mutex_unlock(&op->raw_read_mutex);
                return -EIO;
        }

        while (size) {
                off_t src_off = 0;
                struct volume_handle *vh = NULL;
                if (op->entry_p->flags.multipart) {
                        int vol;

                        __get_vol_and_chunk_raw(op, offset, &vol, &chunk);

                        /* keep current open file */
                        if (vol != op->vno) {
                                /* close/open */
                                op->vno = vol;
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
                                                goto read_error;
                                        }
                                        fclose(op->fp);
                                        op->fp = fp;
                                        force_seek = 1;
                                } else {
                                        pthread_mutex_unlock(&op->raw_read_mutex);
                                        return -EINVAL;
                                }
                        } else {
                                fp = op->fp;
                        }

                        if (force_seek || offset != op->pos) {
                                src_off = VOL_REAL_SZ(vol) - chunk;
                                printd(3, "SEEK src_off = %" PRIu64 ", "
                                                "VOL_REAL_SZ = %" PRIu64 "\n",
                                                src_off, VOL_REAL_SZ(vol));
                                if (fseeko(fp, src_off, SEEK_SET))
                                        goto read_error;
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
                                if (fseeko(fp, src_off, SEEK_SET))
                                        goto read_error;
                        }
                }
                n = fread(buf, 1, chunk, fp);
                if (ferror(fp))
                        goto read_error;
                printd(3, "Read %zu bytes from vol=%d, base=%d\n", n, op->vno,
                       op->entry_p->vno_base);
                if (n != (ssize_t)chunk)
                        size = n;

                size -= n;
                offset += n;
                buf += n;
                tot += n;
                op->pos = offset;
                if (vh)
                        vh->pos += n;
        }
        pthread_mutex_unlock(&op->raw_read_mutex);
        return tot;

read_error:
        if (fp)
                clearerr(fp);
        pthread_mutex_unlock(&op->raw_read_mutex);
        memset(buf, 0, size);
        return tot + size;
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
            int c = wide_to_utf8(wcb->data, buf, size);
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
        uint64_t o = ntoh64(op->buf->idx.data_p->head.offset);
        uint64_t s = ntoh64(op->buf->idx.data_p->head.size);
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
/* This is a workaround for a misbehaving pread(2) on Cygwin (!?).
 * At EOF pread(2) should return 0 but on Cygwin that is not the case and
 * instead some very high number is observed like 1628127168.
 * Let's assume that if what is returned from pread(2) is greater than
 * the requested read size, EOF has been reached.
 * Without this workaround at least dokan-fuse gets very confused and is
 * caught in an endless loop! */
#ifdef __CYGWIN__
        if (res > (int)size)
                return 0;
#endif
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
                    offset >= (off_t)ntoh64(op->buf->idx.data_p->head.offset)) {
                        n = lread_rar_idx(buf, size, offset, op);
                        goto out;
                }
                /* Check for backward read */
                if (offset < op->pos) {
                        printd(3, "seq=%d    history access    offset=%" PRIu64
                                                " size=%zu  op->pos=%" PRIu64
                                                "  split=%d\n",
                                                op->seq, offset, size,
                                                op->pos,
                                                (offset + (off_t)size) > op->pos);
                        if ((uint32_t)(op->pos - offset) <= IOB_HIST_SZ) {
                                size_t pos = offset & (IOB_SZ-1);
                                size_t chunk = (off_t)(offset + size) > op->pos
                                        ? (size_t)(op->pos - offset)
                                        : size;
                                size_t tmp = iob_copy(buf, op->buf, chunk, pos);
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
                                pthread_rwlock_wrlock(&file_access_lock);
                                e_p = filecache_get(FH_TOPATH(fi->fh));
                                if (e_p)
                                        e_p->flags.save_eof = 0;
                                pthread_rwlock_unlock(&file_access_lock);
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
                        pthread_rwlock_wrlock(&file_access_lock);
                        e_p = filecache_get(FH_TOPATH(fi->fh));
                        if (e_p)
                                e_p->flags.direct_io = 1;
                        pthread_rwlock_unlock(&file_access_lock);
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
                off_t offset_saved = op->buf->offset;
                if (sync_thread_read(op))
                        return -EIO;
                /* If there is still no data assume something went wrong.
                 * I/O buffer might simply be full and cannot receive more
                 * data or otherwise most likely CRC errors or an invalid
                 * password in the case of encrypted archives.
                 */
                if (op->buf->offset == offset_saved && !iob_full(op->buf))
                        return -EIO;
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
                                pthread_rwlock_wrlock(&file_access_lock);
                                e_p = filecache_get(FH_TOPATH(fi->fh));
                                if (e_p)
                                        e_p->flags.direct_io = 1;
                                pthread_rwlock_unlock(&file_access_lock);
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
                        (void)iob_write(op->buf, op->fp, IOB_SAVE_HIST);
                        sched_yield();
                }

                if (!feof(op->fp)) {
                        op->buf->ri = offset & (IOB_SZ - 1);
                        op->buf->used -= (offset - op->pos);
                        op->pos = offset;

                        /* Pull in rest of data if needed */
                        if ((size_t)(op->buf->offset - offset) < size)
                                (void)iob_write(op->buf, op->fp,
                                                IOB_SAVE_HIST);
                }
        }

        if (size) {
                int off = offset - op->pos;
                n += iob_read(buf, op->buf, size, off);
                op->pos += (off + size);
                if (__wake_thread(op, RD_ASYNC_READ))
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

        if (FH_TOIO(fi->fh)->type == IO_TYPE_INFO) {
                free(FH_TOPATH(fi->fh));
                free(FH_TOBUF(fi->fh));
        }
        else if (FH_TOFD(fi->fh))
                close(FH_TOFD(fi->fh));
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
/* This is a workaround for a misbehaving pread(2) on Cygwin (!?).
 * At EOF pread(2) should return 0 but on Cygwin that is not the case and
 * instead some very high number is observed like 1628127168.
 * Let's assume that if what is returned from pread(2) is greater than
 * the requested read size, EOF has been reached.
 * Without this workaround at least dokan-fuse gets very confused and is
 * caught in an endless loop! */
#ifdef __CYGWIN__
        if (res > (int)size)
                return 0;
#endif
        return res;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int lopen(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);
        struct io_handle *io = malloc(sizeof(struct io_handle));
        if (!io)
                return -ENOMEM;
        int fd = open(path, fi->flags);
        if (fd == -1) {
                free(io);
                return -errno;
        }
        FH_SETIO(fi->fh, io);
        FH_SETTYPE(fi->fh, IO_TYPE_NRM);
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
static int is_first_volume_by_name(const char *arch)
{
        RAROpenArchiveDataEx d;
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = (char *)arch;   /* Horrible cast! But hey... it is the API! */
        d.OpenMode = RAR_OM_LIST;
        HANDLE hdl = RAROpenArchiveEx(&d);
        int first = 0;

        /* Check for fault */
        if (d.OpenResult && hdl)
                goto out;
        if (!(d.Flags & ROADF_VOLUME))
                first = 1;
        else if((d.Flags & ROADF_VOLUME) && (d.Flags & ROADF_FIRSTVOLUME))
                first = 1;

out:
        RARCloseArchive(hdl);

        return first;
}

/*!
 *****************************************************************************
 * Checks if archive file |arch| is part of a multipart archive.
 * Identifies all the files that are part of the same multipart archive and
 * located in the same directory as |arch| and stores their paths.
 *
 * Returns 0 on success.
 * Returns a negative ERAR_ error code in case of error.
 ****************************************************************************/
static int collect_files(const char *arch)
{
        RAROpenArchiveDataEx d;
        struct RARHeaderDataEx header;
        char *arch_;
        struct dir_entry_list *list;

        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = (char *)arch;   /* Horrible cast! But hey... it is the API! */
        d.OpenMode = RAR_OM_LIST_INCSPLIT;
        d.Callback = list_callback_noswitch;
        d.UserData = (LPARAM)arch;
        HANDLE h;

        arch_ = strdup(arch);
        if (!arch_)
                return -ERAR_NO_MEMORY;

        h = RAROpenArchiveEx(&d);

        /* Check for fault */
        if (d.OpenResult != ERAR_SUCCESS) {
                if (h)
                        RARCloseArchive(h);
                free(arch_);
                return -d.OpenResult;
        }

        if (d.Flags & ROADF_VOLUME) {
                int format = IS_NNN(arch_) ? 1 : VTYPE(d.Flags);
                if (__RARVolNameToFirstName(arch_, !format)) {
                        free(arch_);
                        return -ERAR_EOPEN;
                }
                RARCloseArchive(h);
                d.ArcName = (char *)arch_;
                h = RAROpenArchiveEx(&d);

                /* Check for fault */
                if (d.OpenResult != ERAR_SUCCESS) {
                        if (h)
                                RARCloseArchive(h);
                        free(arch_);
                        return -d.OpenResult;
                }
        }

        RARArchiveDataEx *arc = NULL;
        int dll_result = RARListArchiveEx(h, &arc);
        if (dll_result != ERAR_SUCCESS) {
                if (dll_result == ERAR_EOPEN && arc)
                        dll_result = ERAR_SUCCESS;
                if (dll_result == ERAR_END_ARCHIVE && !arc)
                        dll_result = ERAR_EOPEN;
        }
        if (dll_result != ERAR_SUCCESS && dll_result != ERAR_END_ARCHIVE) {
                RARFreeArchiveDataEx(&arc);
                RARCloseArchive(h);
                free(arch_);
                return -dll_result;
        }

        /* Pointless to test for encrypted files if header is already encrypted
         * and could be read. */
        if (d.Flags & ROADF_ENCHEADERS)
                goto skip_file_check;
        if (arc->hdr.Flags & RHDF_ENCRYPTED) {
                dll_result = extract_rar(arch_, arc->hdr.FileName, NULL);
                if (dll_result != ERAR_SUCCESS && dll_result != ERAR_UNKNOWN) {
                        RARFreeArchiveDataEx(&arc);
                        RARCloseArchive(h);
                        free(arch_);
                        return -dll_result;
                }
        }

skip_file_check:
        RARFreeArchiveDataEx(&arc);
        RARCloseArchive(h);

        list = arch_list;
        dir_list_open(list);

        /* Let libunrar deal with the collection of volume parts */
        if (d.Flags & ROADF_VOLUME) {
                h = RAROpenArchiveEx(&d);

                /* Check for fault */
                if (d.OpenResult != ERAR_SUCCESS) {
                        if (h)
                                RARCloseArchive(h);
                        free(arch_);
                        return -d.OpenResult;
                }
                while (1) {
                        dll_result = RARReadHeaderEx(h, &header);
                        if (dll_result != ERAR_SUCCESS) {
                                if (dll_result == ERAR_END_ARCHIVE)
                                        dll_result = ERAR_SUCCESS;
                                else
                                        dll_result = ERAR_EOPEN;
                                break;
                        }
                        (void)RARProcessFile(h, RAR_SKIP, NULL, NULL);
                        list = dir_entry_add(list, header.ArcName, NULL,
                                             DIR_E_NRM);
                }
                RARCloseArchive(h);
        } else {
                (void)dir_entry_add(list, arch_, NULL, DIR_E_NRM);
                dll_result = ERAR_SUCCESS;
        }

        if (dll_result != ERAR_SUCCESS)
                dir_list_free(arch_list);
        free(arch_);

        /* Do not close the list since it could re-order the entries! */
        return -dll_result;
}

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
        memset(&header, 0, sizeof(header));
        HANDLE hdl = 0;
        struct idx_head head = {R2I_MAGIC, R2I_VERSION, 0, 0, 0};
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
                /* Handle the special case when asking for a quick "dry run"
                 * to test archive integrity. If all is well this will result
                 * in an ERAR_UNKNOWN error. */
                if (!cb_arg->arg) {
                        if (!cb_arg->dry_run) {
                                cb_arg->dry_run = 1;
                                return 1;
                        }
                        return -1;
                }
                /*
                 * We do not need to handle the case that not all data is
                 * written after return from write() since the pipe is not
                 * opened using the O_NONBLOCK flag.
                 */
                if (write((LPARAM)cb_arg->arg, (void *)P1, P2) == -1) {
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
        cb_arg.dry_run = 0;

        d.Callback = extract_callback;
        d.UserData = (LPARAM)&cb_arg;
        struct RARHeaderDataEx header;
        memset(&header, 0, sizeof(header));
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
 * For setting high-precision timestamp, used by set_rarstats()
 ****************************************************************************/
#if defined(HAVE_STRUCT_STAT_ST_MTIM) || defined(HAVE_STRUCT_STAT_ST_CTIM) || defined(HAVE_STRUCT_STAT_ST_ATIM)
void set_high_precision_ts(struct timespec *spec, uint64_t stamp)
{
/* libunrar 5.5.x and later provides 1 ns resolution UNIX timestamp */
#if RARVER_MAJOR > 5 || (RARVER_MAJOR == 5 && RARVER_MINOR >= 50)
        spec->tv_sec  = (stamp / 1000000000);
        spec->tv_nsec = (stamp % 1000000000);
/* Earlier versions provide function GetRaw(), 100 ns resolution
 * Windows timestamp. */
#else
        spec->tv_sec  = (stamp / 10000000);
        spec->tv_nsec = (stamp % 10000000) * 100;
#endif
}
#endif

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static uint64_t extract_file_size(char *arch, const char *file)
{
        struct RAROpenArchiveDataEx d;
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = arch;
        d.OpenMode = RAR_OM_LIST_INCSPLIT;
        d.Callback = list_callback;
        d.UserData = (LPARAM)arch;
        struct RARHeaderDataEx header;
        memset(&header, 0, sizeof(header));
        HANDLE hdl = RAROpenArchiveEx(&d);
        uint64_t size = 0;

        if (d.OpenResult)
                goto out;

        header.CmtBufSize = 0;
        while (1) {
                if (RARReadHeaderEx(hdl, &header))
                        break;
                if (!strcmp(header.FileName, file)) {
                        /* Since some archives seems to have corrupt information
                         * in the uncompressed size, use the accumulated
                         * compressed size instead. It should anyway be the same
                         * for archives in store mode (-m0). For archives in
                         * compressed mode we must trust what is there as the
                         * uncompressed size. */
                        if (header.Method == FHD_STORING &&
                                        !IS_RAR_DIR(&header)) {
                                size += GET_RAR_PACK_SZ(&header);
                        } else {
                                size = GET_RAR_SZ(&header);
                                break;
                        }
                }
                if (RARProcessFile(hdl, RAR_SKIP, NULL, NULL))
                        break;
        }

out:
        if (hdl)
                RARCloseArchive(hdl);

        return size;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void set_rarstats(struct filecache_entry *entry_p, RARArchiveDataEx *arc,
                         int force_dir)
{
	off_t st_size;

        if (!force_dir) {
                st_size = GET_RAR_SZ(&arc->hdr);
                /* Handle the one case discovered so far with archives having
                 * bad size information in the header. */
                if ((st_size == INT64NDF) && entry_p->flags.raw)
                        st_size = extract_file_size(entry_p->rar_p,
                                                    entry_p->file_p);
                mode_t mode = GET_RAR_MODE(&arc->hdr);
                if (!S_ISDIR(mode) && !S_ISLNK(mode)) {
                        /* Force file to be treated as a 'regular file' */
                        mode = (mode & ~S_IFMT) | S_IFREG;
                }
                if (S_ISLNK(mode)) {
                        if (arc->LinkTargetFlags & LINK_T_UNICODE) {
                                char *tmp = malloc(sizeof(arc->LinkTarget));
                                if (tmp) {
                                        size_t len = wide_to_utf8(
                                                arc->LinkTargetW, tmp,
                                                sizeof(arc->LinkTarget));
                                        if ((int)len != -1) {
                                                entry_p->link_target_p = strdup(tmp);
                                                st_size = len;
                                        }
                                        free(tmp);
                                }
                        } else {
                                entry_p->link_target_p =
                                        strdup(arc->LinkTarget);
                        }
                }
                if (OPT_SET(OPT_KEY_NO_INHERIT_PERM)) {
			if (S_ISDIR(mode))
			    mode = (mode & S_IFMT) | (0777 & ~umask_);
			else
			    mode = (mode & S_IFMT) | (0666 & ~umask_);
                }
                entry_p->stat.st_mode = mode;
#ifndef HAVE_SETXATTR
                entry_p->stat.st_nlink =
                        S_ISDIR(mode) ? 2 : arc->hdr.Method - (FHD_STORING - 1);
#else
                entry_p->stat.st_nlink =
                        S_ISDIR(mode) ? 2 : 1;
#endif
        } else {
                entry_p->stat.st_mode = (S_IFDIR | (0777 & ~umask_));
                entry_p->stat.st_nlink = 2;
                st_size = 4096;
        }
        entry_p->stat.st_uid = getuid();
        entry_p->stat.st_gid = getgid();
        entry_p->stat.st_ino = 0;
        entry_p->stat.st_size = st_size;

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

        if (!OPT_SET(OPT_KEY_DATE_RAR)) {
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
                         * Avoid type-punned pointer warning when strict
                         * aliasing is used with some versions of gcc.
                         */
                        unsigned int as_uint_;
                };

                /* Using DOS time format by default for backward compatibility. */
                union dos_time_t *dos_time =
                                (union dos_time_t *)&arc->hdr.FileTime;

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

                /* Set internally stored high precision timestamp if available. */
#ifdef HAVE_STRUCT_STAT_ST_MTIM
                if (arc->RawTime.mtime)
                        set_high_precision_ts(&entry_p->stat.st_mtim,
                                                arc->RawTime.mtime);
#endif
#ifdef HAVE_STRUCT_STAT_ST_CTIM
                if (arc->RawTime.ctime)
                        set_high_precision_ts(&entry_p->stat.st_ctim,
                                                arc->RawTime.ctime);
#endif
#ifdef HAVE_STRUCT_STAT_ST_ATIM
                if (arc->RawTime.atime)
                        set_high_precision_ts(&entry_p->stat.st_atim,
                                                arc->RawTime.atime);
#endif
        } else {
                struct stat st;
                stat(entry_p->rar_p, &st);
                entry_p->stat.st_atime = st.st_atime;
                entry_p->stat.st_mtime = st.st_mtime;
                entry_p->stat.st_ctime = st.st_ctime;
#ifdef HAVE_STRUCT_STAT_ST_ATIM
                entry_p->stat.st_atim.tv_nsec = st.st_atim.tv_nsec;
#endif
#ifdef HAVE_STRUCT_STAT_ST_MTIM
                entry_p->stat.st_mtim.tv_nsec = st.st_mtim.tv_nsec;
#endif
#ifdef HAVE_STRUCT_STAT_ST_CTIM
                entry_p->stat.st_ctim.tv_nsec = st.st_ctim.tv_nsec;
#endif
        }
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
                RARArchiveDataEx *arc,
                const char *rar_root, int display)

{
        struct filecache_entry *e_p = NULL;
        char *tmp = malloc(sizeof(arc->LinkTarget));
        if (tmp) {
                if (wide_to_utf8(arc->LinkTargetW, tmp,
                                 sizeof(arc->LinkTarget)) != (size_t)-1) {
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
                if (!match || match != file || path_len == file_len ||
                    file[path_len] != '/')
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
static void __listrar_incache(struct filecache_entry *entry_p,
                RARArchiveDataEx *arc)
{
        if (!entry_p->flags.vsize_resolved) {
              entry_p->vsize_next = GET_RAR_PACK_SZ(&arc->hdr);
              if (((arc->hdr.Flags & RHDF_SPLITAFTER) && entry_p->vsize_next) ||
                              /* Handle files located in only two volumes */
                              (entry_p->vsize_first + entry_p->vsize_next) ==
                                      entry_p->stat.st_size)
                      entry_p->flags.vsize_resolved = 1;
              else
                      goto vsize_done;
              entry_p->vsize_real_next = arc->FileDataEnd;
              /* Check if we might need to compensate for the 1-byte/2-byte
               * RAR5 (and later?) volume number in next main archive header. */
              if (arc->hdr.UnpVer >= 50) {
                       /* If base is last or next to last volume with one extra
                        * byte in header this and next volume size have already
                        * been resolved. */
                      if ((entry_p->vno_base - entry_p->vno_first + 1) < 128) {
                              if (entry_p->stat.st_size >
                                            (entry_p->vsize_first +
                                            (entry_p->vsize_next *
                                                    (128 - (entry_p->vno_base - entry_p->vno_first + 1)))))
                                      entry_p->flags.vsize_fixup_needed = 1;
                      }
              }
        }

vsize_done:
        /*
         * Check if this was a forced/fake entry. In that case update it
         * with proper stats.
         */
        if (entry_p->flags.force_dir) {
                set_rarstats(entry_p, arc, 0);
                entry_p->flags.force_dir = 0;
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static struct filecache_entry *__listrar_tocache(char *file,
                RARArchiveDataEx *arc, const char *arch, char *first_arch,
                RAROpenArchiveDataEx *d)
{
        struct filecache_entry *entry_p;
        int raw_mode;

        if (arc->hdr.Method == FHD_STORING &&
                        !(arc->hdr.Flags & RHDF_ENCRYPTED) &&
                        !IS_RAR_DIR(&arc->hdr)) {
                if (arc->hdr.Flags & RHDF_SPLITBEFORE)
                        return NULL;
                raw_mode = 1;
        } else {
                raw_mode = 0;
        }

        /* Allocate a cache entry for this file */
        printd(3, "Adding %s to cache\n", file);
        entry_p = filecache_alloc(file);

        entry_p->rar_p = strdup(first_arch);
        entry_p->file_p = strdup(arc->hdr.FileName);
        entry_p->flags.vsize_resolved = 1; /* Assume sizes will be resolved */
        if (IS_RAR_DIR(&arc->hdr))
                entry_p->flags.unresolved = 0;
        else
                entry_p->flags.unresolved = 1;

        if (raw_mode) {
                entry_p->flags.raw = 1;
                if ((d->Flags & ROADF_VOLUME)) {   /* volume ? */
                        int len, pos;

                        entry_p->flags.multipart = 1;
                        entry_p->vtype = IS_NNN(arch) ? 1 : VTYPE(d->Flags);
                        entry_p->vno_base = get_vformat(arch,
                                        entry_p->vtype, &len, &pos);
                        entry_p->vno_first = get_vformat(entry_p->rar_p,
                                        entry_p->vtype, NULL, NULL);
                        if (len > 0) {
                                entry_p->vlen = len;
                                entry_p->vpos = pos;
                                if (!IS_RAR_DIR(&arc->hdr)) {
                                        entry_p->vsize_real_first = arc->FileDataEnd;
                                        entry_p->vsize_first = GET_RAR_PACK_SZ(&arc->hdr);
                                        /*
                                         * Assume next volume to hold same amount
                                         * of data as the first. It will be adjusted
                                         * later if needed.
                                         */
                                        entry_p->vsize_next = entry_p->vsize_first;
                                        if (arc->hdr.Flags & (RHDF_SPLITBEFORE | RHDF_SPLITAFTER))
                                                entry_p->flags.vsize_resolved = 0;
                                }
                        } else {
                                entry_p->flags.raw = 0;
                                entry_p->flags.save_eof =
                                                get_save_eof(entry_p->rar_p);
                        }
                } else {
                        entry_p->flags.multipart = 0;
                        entry_p->offset = (arc->Offset + arc->HeadSize);
                }
        } else {        /* Folder or Compressed and/or Encrypted */
                entry_p->flags.raw = 0;
                /* Check if part of a volume */
                if (d->Flags & ROADF_VOLUME) {
                        entry_p->flags.multipart = 1;
                        entry_p->vtype = IS_NNN(arch) ? 1 : VTYPE(d->Flags);
                } else {
                        entry_p->flags.multipart = 0;
                }
                if (!IS_RAR_DIR(&arc->hdr)) {
                        entry_p->flags.save_eof = get_save_eof(entry_p->rar_p);
                        if (arc->hdr.Flags & RHDF_ENCRYPTED)
                                entry_p->flags.encrypted = 1;
                }
        }
        entry_p->method = arc->hdr.Method;
        set_rarstats(entry_p, arc, 0);

        return entry_p;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __listrar_tocache_forcedir(struct filecache_entry *entry_p,
                RARArchiveDataEx *arc, const char *file, char *first_arch,
                RAROpenArchiveDataEx *d)
{
        entry_p->rar_p = strdup(first_arch);
        entry_p->file_p = strdup(file);
        entry_p->flags.force_dir = 1;
        entry_p->flags.unresolved = 0;

        set_rarstats(entry_p, arc, 1);

        /* Check if part of a volume */
        if (d->Flags & ROADF_VOLUME) {
                entry_p->flags.multipart = 1;
                entry_p->vtype = IS_NNN(first_arch) ? 1 : VTYPE(d->Flags);
        } else {
                entry_p->flags.multipart = 0;
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static inline void __listrar_cachedir(const char *mp)
{
        pthread_rwlock_wrlock(&dir_access_lock);
        if (!dircache_get(mp))
		(void)dircache_alloc(mp);
        pthread_rwlock_unlock(&dir_access_lock);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __listrar_cachedirentry(const char *mp)
{
        char *safe_path = strdup(mp);
        char *tmp = safe_path;
        safe_path = __gnu_dirname(safe_path);
        if (CHRCMP(safe_path, '/')) {
                pthread_rwlock_wrlock(&dir_access_lock);
                struct dircache_entry *dce = dircache_get(safe_path);
                if (dce) {
                        char *tmp2 = strdup(mp);
                        dir_entry_add(&dce->dir_entry_list, basename(tmp2),
                                      NULL, DIR_E_RAR);
                        free(tmp2);
                }
                pthread_rwlock_unlock(&dir_access_lock);
        }
        free(tmp);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int listrar(const char *path, struct dir_entry_list **buffer,
                const char *arch, char **first_arch, int *final)
{
        ENTER_("%s   arch=%s", path, arch);
        RAROpenArchiveDataEx d;
        memset(&d, 0, sizeof(RAROpenArchiveDataEx));
        d.ArcName = (char *)arch;   /* Horrible cast! But hey... it is the API! */
        d.OpenMode = RAR_OM_LIST_INCSPLIT;
        d.Callback = list_callback_noswitch;
        d.UserData = (LPARAM)arch;
        HANDLE hdl = RAROpenArchiveEx(&d);

        /* Check for fault */
        if (d.OpenResult) {
                if (hdl)
                        RARCloseArchive(hdl);
                return d.OpenResult;
        }

        if (d.Flags & ROADF_ENCHEADERS) {
                RARCloseArchive(hdl);
                d.Callback = list_callback;
                hdl = RAROpenArchiveEx(&d);
                if (final)
                        *final = 1;
        }

        char *tmp1 = strdup(arch);
        char *rar_root = strdup(__gnu_dirname(tmp1));
        free(tmp1);
        tmp1 = rar_root;
        rar_root += strlen(OPT_STR2(OPT_KEY_SRC, 0));
        int is_root_path = (!strcmp(rar_root, path) || !CHRCMP(path, '/'));
        int ret = 0;
        RARArchiveDataEx *arc = NULL;

        if (*first_arch == NULL) {
                /* The caller is responsible for freeing this! */
                *first_arch = strdup(arch);

                /* Make sure parent folders are always searched from the first
                 * volume file since sub-folders might actually be placed
                 * elsewhere. Also the alias function depends on this. */
                if ((d.Flags & ROADF_VOLUME) && !(d.Flags & ROADF_FIRSTVOLUME)
                                && __RARVolNameToFirstName(
                                           *first_arch, !VTYPE(d.Flags))) {
                        free(*first_arch);
                        *first_arch = NULL;
                        goto out;
                }
        }

        int dll_result = ERAR_SUCCESS;
        while (dll_result == ERAR_SUCCESS) {
                if ((dll_result = RARListArchiveEx(hdl, &arc))) {
                        if (dll_result != ERAR_EOPEN) {
                                if (dll_result != ERAR_END_ARCHIVE)
                                        ret = 1;
                                continue;
                        }
                }

                char *mp;
                int display = 0;

                DOS_TO_UNIX_PATH(arc->hdr.FileName);

                pthread_rwlock_wrlock(&file_access_lock);

                /* Handle the case when the parent folders do not have
                 * their own entry in the file header or is located in
                 * the end. The entries needs to be faked by adding it
                 * to the cache. If the parent folder is discovered
                 * later in the header the faked entry will be
                 * invalidated and replaced with the real file stats. */
                if (is_root_path) {
                        int populate_cache = 0;
                        char *safe_path = strdup(arc->hdr.FileName);
                        char *tmp = safe_path;
                        while (1) {
                                char *mp2;

                                safe_path = __gnu_dirname(safe_path);
                                if (!CHRCMP(safe_path, '.'))
                                        break;

                                ABS_MP2(mp2, path, safe_path);
                                struct filecache_entry *entry_p = filecache_get(mp2);
                                if (entry_p == NULL) {
                                        printd(3, "Adding %s to cache\n", mp2);
                                        entry_p = filecache_alloc(mp2);
                                        __listrar_tocache_forcedir(entry_p, arc,
                                                        safe_path, *first_arch, &d);
                                        __listrar_cachedir(mp2);
                                        populate_cache = 1;
                                }
                                free(mp2);
                        }
                        free(tmp);
                        if (populate_cache) {
                                /* Entries have been forced into the cache.
                                 * Add the child node to each entry. */
                                safe_path = strdup(arc->hdr.FileName);
                                tmp = safe_path;
                                while (1) {
                                        char *mp2;

                                        safe_path = __gnu_dirname(safe_path);
                                        if (!CHRCMP(safe_path, '.'))
                                                break;

                                        ABS_MP2(mp2, path, safe_path);
                                        __listrar_cachedirentry(mp2);
                                        free(mp2);
                               }
                               free(tmp);
                       }
                }

                /* Aliasing is not support for directories */
                if (!IS_RAR_DIR(&arc->hdr))
                        ABS_MP2(mp, (*rar_root ? rar_root : "/"),
                                        get_alias(*first_arch, arc->hdr.FileName));
                else
                        ABS_MP2(mp, (*rar_root ? rar_root : "/"),
                                        arc->hdr.FileName);

                printd(3, "Looking up %s in cache\n", mp);
                struct filecache_entry *entry_p = filecache_get(mp);
                if (entry_p)  {
                        __listrar_incache(entry_p, arc);
                        goto cache_hit;
                }

                if (arc->LinkTargetFlags & LINK_T_FILECOPY) {
                        struct filecache_entry *e_p;
                        e_p = lookup_filecopy(path, arc, rar_root, display);
                        if (e_p) {
                                printd(3, "Adding %s to cache\n", mp);
                                entry_p = filecache_alloc(mp);
                                filecache_copy(e_p, entry_p);
                                /* Preserve stats of original file */
                                set_rarstats(entry_p, arc, 0);
                                goto cache_hit;
                        }
                }

                entry_p = __listrar_tocache(mp, arc, arch, *first_arch, &d);
                if (entry_p == NULL) {
                        pthread_rwlock_unlock(&file_access_lock);
                        free(mp);
                        continue;
                }

cache_hit:
                pthread_rwlock_unlock(&file_access_lock);
                __add_filler(path, buffer, mp);
                if (IS_RAR_DIR(&arc->hdr))
                        __listrar_cachedir(mp);
                __listrar_cachedirentry(mp);
                free(mp);
        }

out:
        RARFreeArchiveDataEx(&arc);
        RARCloseArchive(hdl);
        free(tmp1);

        return ret;
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

struct filter_ops {
        int (*filter[3]) (SCANDIR_ARG3);
        unsigned int f_end;
        unsigned int f_nrm;
        unsigned int f_rar;
        unsigned int f_rxx;
};

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __resolve_dir(const char *dir, const char *root,
                struct dir_entry_list **next,
                struct dir_entry_list **next2,
                struct filter_ops *f_ops)
{
        struct dirent **namelist = NULL;
        unsigned int f;
        int error_tot = 0;
        int seek_len = 0;
        char *first_arch = NULL;
        int ret = 0;

        for (f = 0; f < f_ops->f_end; f++) {
                off_t prev_size = 0;
                size_t prev_len = 0;
                int reset = 1;
                int error_cnt = 0;
                int final = 0;
                int vno = 0;
                int vcnt = 0;
                int i = 0;
                int n = scandir(root, &namelist, f_ops->filter[f], alphasort);
                if (n < 0) {
                        perror("scandir");
                        ret = -EIO;
                        goto next_type;
                }
                while (i < n) {
                        int pos = 0;
                        int pos2 = 0;
                        char *arch = NULL;

                        if (f == f_ops->f_nrm && next) {
                                *next = dir_entry_add(*next, namelist[i]->d_name,
                                                      NULL, DIR_E_NRM);
                                goto next_entry;
                        }

                        ABS_MP2(arch, root, namelist[i]->d_name);

                        if (f == f_ops->f_rar || f == f_ops->f_rxx) {
                                int oldvno = vno;
                                int len;
                                const char *d_name = namelist[i]->d_name;

                                vno = get_vformat(d_name, f != f_ops->f_rxx,
                                                        &len, &pos);
                                pos2 = pos + len;
                                if (vno <= oldvno)
                                        reset = 1;
                        }
                        if (f == f_ops->f_rar) {
                                struct stat st;
                                size_t len = strlen(namelist[i]->d_name);
                                if (!stat(arch, &st)) {
                                        if (vcnt && !reset) {
                                                if (prev_len != len)
                                                        reset = 1;
                                                else if (strncmp(namelist[i]->d_name,
                                                            namelist[i - 1]->d_name,
                                                            pos))
                                                        reset = 1;
                                                else if (strcmp(namelist[i]->d_name
                                                                    + pos2,
                                                            namelist[i - 1]->d_name
                                                                    + pos2))
                                                        reset = 1;
                                                else if (prev_len != len)
                                                        reset = 1;
                                                else if (st.st_size != (long)prev_size)
                                                        if (is_first_volume_by_name(arch))
                                                                reset = 1;
                                        }
                                        prev_size = st.st_size;
                                } else {
                                        free(arch);
                                        ret = -EIO;
                                        goto next_type;
                                }
                                prev_len = len;
                        }

                        if (reset) {
                                error_cnt = 0;
                                final = 0;
                                reset = 0;
                                seek_len = get_seek_length(arch);
                                /* We always need to scan at least two volume files */
                                seek_len = seek_len == 1 ? 2 : seek_len;
                                free(first_arch);
                                first_arch = NULL;
                                vcnt = f == f_ops->f_rxx;
                        }

                        if (!seek_len || vcnt < seek_len) {
                                ++vcnt;
                                if (!final && !error_cnt) {
                                        if (listrar(dir, next2, arch,
                                                    &first_arch, &final)) {
                                                ++error_tot;
                                                ++error_cnt;
                                        }
                                }
                        }
                        if (error_cnt && next)
                                *next = dir_entry_add(*next, namelist[i]->d_name,
                                                      NULL, DIR_E_NRM);
                        free(arch);
                        arch = NULL;

next_entry:
                        ++i;
                }

next_type:
                if (namelist) {
                        for (i = 0; i < n; i++)
                                free(namelist[i]);
                        free(namelist);
                        namelist = NULL;
                }

                if (ret)
                        break;
        }

        free(first_arch);

        return ret < 0 ? ret : error_tot;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int syncdir_scan(const char *dir, const char *root,
                struct dir_entry_list **next)
{
        struct filter_ops f_ops;

        ENTER_("%s", dir);

        f_ops.filter[0] = f1;
        f_ops.filter[1] = f2;
        f_ops.f_end = 2;
        f_ops.f_nrm = ~0;
        f_ops.f_rar = 0;
        f_ops.f_rxx = 1;

        return __resolve_dir(dir, root, NULL, next, &f_ops);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int readdir_scan(const char *dir, const char *root,
                struct dir_entry_list **next,
                struct dir_entry_list **next2)
{
        struct filter_ops f_ops;

        ENTER_("%s", dir);

        if (*next2) {
                f_ops.filter[0] = f0;
                f_ops.filter[1] = f1;
                f_ops.filter[2] = f2;
                f_ops.f_end = 3;
                f_ops.f_nrm = 0;
                f_ops.f_rar = 1;
                f_ops.f_rxx = 2;
        } else {
                /* New RAR files will not be displayed if the cache is in
                 * effect. Optionally the entry list could be scanned for
                 * matching filenames and display only those not already
                 * cached. That would however affect the performance in the
                 * normal case too and currently the choice is simply to
                 * ignore such files. */
                f_ops.filter[0] = f0;
                f_ops.f_end = 1;
                f_ops.f_nrm = 0;
                f_ops.f_rar = ~0;
                f_ops.f_rxx = ~0;
        }

        return __resolve_dir(dir, root, next, next2, &f_ops);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int syncdir(const char *path)
{
        ENTER_("%s", path);

        DIR *dp;
        char *root;
        struct dircache_entry *entry_p;
        struct dir_entry_list *dir_list; /* internal list root */
        struct dir_entry_list *next;

        pthread_rwlock_rdlock(&dir_access_lock);
        entry_p = dircache_get(path);
        pthread_rwlock_unlock(&dir_access_lock);
        if (entry_p)
                return 0;

        ABS_ROOT(root, path);
        dp = opendir(root);
        if (dp != NULL) {
                int res;

                dir_list = malloc(sizeof(struct dir_entry_list));
                next = dir_list;
                if (!next) {
                        closedir(dp);
                        return -ENOMEM;
                }
                dir_list_open(next);
                res = syncdir_scan(path, root, &next);
                (void)closedir(dp);
                if (res) {
                        dir_list_free(dir_list);
                        free(dir_list);
                        return res < 0 ? res : 0;
                }

                pthread_rwlock_wrlock(&dir_access_lock);
                entry_p = dircache_alloc(path);
                if (entry_p)
                        entry_p->dir_entry_list = *dir_list;
                free(dir_list);
                pthread_rwlock_unlock(&dir_access_lock);
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
        char *first_arch;

        pthread_rwlock_rdlock(&dir_access_lock);
        entry_p = dircache_get(path);
        pthread_rwlock_unlock(&dir_access_lock);
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
        int c_end = get_seek_length(NULL);
        c_end = c_end ? c_end == 1 ? 2 : c_end : c_end;
        struct dir_entry_list *arch_next = arch_list_root.next;

        dir_list_open(dir_list);
        first_arch = arch_next->entry.name;
        while (arch_next) {
                (void)listrar(path, &next, arch_next->entry.name,
                                        &first_arch, &final);
                if ((++c == c_end) || final)
                        break;
                arch_next = arch_next->next;
        }

        pthread_rwlock_wrlock(&dir_access_lock);
        entry_p = dircache_alloc(path);
        if (entry_p)
                entry_p->dir_entry_list = *dir_list;
        pthread_rwlock_unlock(&dir_access_lock);

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

        pthread_rwlock_rdlock(&file_access_lock);
        entry_p = path_lookup(path, stbuf);
        if (entry_p) {
                if (entry_p != LOOP_FS_ENTRY) {
                        pthread_rwlock_unlock(&file_access_lock);
                        dump_stat(stbuf);
                        return 0;
                }
                pthread_rwlock_unlock(&file_access_lock);
                return -ENOENT;
        }
        pthread_rwlock_unlock(&file_access_lock);

        /*
         * There was a cache miss and the file could not be found locally!
         * This is bad! To make sure the files does not really exist all
         * rar archives need to be scanned for a matching file = slow!
         */
        if (OPT_FILTER(path))
                return -ENOENT;
        char *safe_path = strdup(path);
        char *tmp = safe_path;
        while (1) {
                safe_path = __gnu_dirname(safe_path);
                syncdir(safe_path);
                if (*safe_path == '/')
                        break;
        }
        free(tmp);

        pthread_rwlock_rdlock(&file_access_lock);
        entry_p = path_lookup(path, stbuf);
        if (entry_p) {
                pthread_rwlock_unlock(&file_access_lock);
                dump_stat(stbuf);
                return 0;
        }
        pthread_rwlock_unlock(&file_access_lock);

#if RARVER_MAJOR > 4
        int cmd = 0;
        while (file_cmd[cmd]) {
                size_t len_path = strlen(path);
                size_t len_cmd = strlen(file_cmd[cmd]);
                if (len_path > len_cmd &&
                                !strcmp(&path[len_path - len_cmd], file_cmd[cmd])) {
                        char *root;
                        char *real = strdup(path);
                        real[len_path - len_cmd] = 0;
                        ABS_ROOT(root, real);
                        if (access(root, F_OK)) {
                                if (filecache_get(real)) {
                                        memset(stbuf, 0, sizeof(struct stat));
                                        stbuf->st_mode = S_IFREG | 0644;
                                        free(real);
                                        return 0;
                                }
                        }
                        free(real);
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

        pthread_rwlock_rdlock(&file_access_lock);
        if (path_lookup(path, stbuf)) {
                pthread_rwlock_unlock(&file_access_lock);
                dump_stat(stbuf);
                return 0;
        }
        pthread_rwlock_unlock(&file_access_lock);

        /*
         * There was a cache miss! To make sure the file does not really
         * exist the rar archive needs to be scanned for a matching file.
         * This should not happen very frequently unless the contents of
         * the rar archive was actually changed after it was mounted.
         */
        res = syncrar("/");
        if (res)
                return res;

        pthread_rwlock_rdlock(&file_access_lock);
        struct filecache_entry *entry_p = path_lookup(path, stbuf);
        if (entry_p) {
                pthread_rwlock_unlock(&file_access_lock);
                dump_stat(stbuf);
                return 0;
        }
        pthread_rwlock_unlock(&file_access_lock);

#if RARVER_MAJOR > 4
        int cmd = 0;
        while (file_cmd[cmd]) {
                size_t len_path = strlen(path);
                size_t len_cmd = strlen(file_cmd[cmd]);
                if (len_path > len_cmd &&
                                !strcmp(&path[len_path - len_cmd], file_cmd[cmd])) {
                        char *root;
                        char *real = strdup(path);
                        real[len_path - len_cmd] = 0;
                        ABS_ROOT(root, real);
                        if (filecache_get(real)) {
                                memset(stbuf, 0, sizeof(struct stat));
                                stbuf->st_mode = S_IFREG | 0644;
                                free(real);
                                return 0;
                        }
                        free(real);
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
                                ABS_MP2(tmp, path, next->entry.name);
                                filecache_invalidate(tmp);
                                free(tmp);
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
static int rar2_opendir2(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", path);

        FH_SETIO(fi->fh, malloc(sizeof(struct io_handle)));
        if (!FH_ISSET(fi->fh))
                return -ENOMEM;
        FH_SETTYPE(fi->fh, IO_TYPE_DIR);
        FH_SETPATH(fi->fh, strdup(path));

        return 0;
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

        int ret = 0;
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
        pthread_rwlock_rdlock(&dir_access_lock);
        struct dircache_entry *entry_p = dircache_get(path);
        if (!entry_p) {
                pthread_rwlock_unlock(&dir_access_lock);
                dir_list2 = malloc(sizeof(struct dir_entry_list));
                if (!dir_list2)
                        return -ENOMEM;
                next2 = dir_list2;
                dir_list_open(dir_list2);
        } else {
                dir_list2 = dir_list_dup(&entry_p->dir_entry_list);
                pthread_rwlock_unlock(&dir_access_lock);
        }

        DIR *dp = FH_TODP(fi->fh);
        if (dp != NULL) {
                char *root;
                if (fs_loop) {
                        if (!strcmp(path, fs_loop_mp_root)) {
				dp = NULL;
                                goto dump_buff;
                        }
                }
                ABS_ROOT(root, path);
                ret = readdir_scan(path, root, &next, &next2);
                if (ret) {
                        __dircache_invalidate(path);
                        goto dump_buff_nocache;
                }
        }

        /* Check if cache is populated */
        if (entry_p)
                goto dump_buff;

        /* It is possible but not very likely that we end up here
         * due to that the cache has not yet been populated.
         * Scan through the entire file path to force a cache update.
         * This is a similar action as required for a cache miss in
         * getattr(). */
        char *safe_path = strdup(path);
        char *tmp = safe_path;
        while (*safe_path != '/') {
                safe_path = __gnu_dirname(safe_path);
                syncdir(safe_path);
        }
        free(tmp);
        pthread_rwlock_rdlock(&dir_access_lock);
        entry_p = dircache_get(path);
        if (entry_p) {
                free(dir_list2);
                dir_list2 = dir_list_dup(&entry_p->dir_entry_list);
        }
        pthread_rwlock_unlock(&dir_access_lock);

dump_buff:

        if (dp == NULL) {
                filler(buffer, ".", NULL, 0);
                filler(buffer, "..", NULL, 0);
        }

        (void)dir_list_append(&dir_list, dir_list2);
        dir_list_close(&dir_list);

        if (!entry_p) {
                pthread_rwlock_wrlock(&dir_access_lock);
                entry_p = dircache_alloc(path);
                if (entry_p)
                        entry_p->dir_entry_list = *dir_list2;
                pthread_rwlock_unlock(&dir_access_lock);
                free(dir_list2);
        } else {
                dir_list_free(dir_list2);
                free(dir_list2);
        }

        dump_dir_list(path, buffer, filler, &dir_list);
        dir_list_free(&dir_list);

        return ret;

dump_buff_nocache:

        (void)dir_list_append(&dir_list, dir_list2);
        dir_list_close(&dir_list);

        dump_dir_list(path, buffer, filler, &dir_list);
        dir_list_free(&dir_list);
        dir_list_free(dir_list2);
        free(dir_list2);

        return ret < 0 ? ret : 0;
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
        pthread_rwlock_rdlock(&dir_access_lock);
        struct dircache_entry *entry_p = dircache_get(path);
        if (!entry_p) {
                int c = 0;
                int final = 0;
                char *first_arch;
                pthread_rwlock_unlock(&dir_access_lock);
                dir_list = malloc(sizeof(struct dir_entry_list));
                struct dir_entry_list *next = dir_list;
                if (!next)
                        return -ENOMEM;

                /* We always need to scan at least two volume files */
                int c_end = get_seek_length(NULL);
                c_end = c_end ? c_end == 1 ? 2 : c_end : c_end;
                struct dir_entry_list *arch_next = arch_list_root.next;

                dir_list_open(next);
                first_arch = arch_next->entry.name;
                while (arch_next) {
                        (void)listrar(FH_TOPATH(fi->fh), &next,
                                                        arch_next->entry.name,
                                                        &first_arch,
                                                        &final);
                        if ((++c == c_end) || final)
                                break;
                        arch_next = arch_next->next;
                }
        } else {
                dir_list = dir_list_dup(&entry_p->dir_entry_list);
                pthread_rwlock_unlock(&dir_access_lock);
        }

        filler(buffer, ".", NULL, 0);
        filler(buffer, "..", NULL, 0);

        dir_list_close(dir_list);
        dump_dir_list(FH_TOPATH(fi->fh), buffer, filler, dir_list);

        if (!entry_p) {
                pthread_rwlock_wrlock(&dir_access_lock);
                entry_p = dircache_alloc(path);
                if (entry_p)
                        entry_p->dir_entry_list = *dir_list;
                pthread_rwlock_unlock(&dir_access_lock);
                free(dir_list);
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
static int rar2_releasedir2(const char *path, struct fuse_file_info *fi)
{
        ENTER_("%s", (path ? path : ""));

        (void)path;

        struct io_handle *io = FH_TOIO(fi->fh);
        if (io == NULL)
                return -EIO;
        free(FH_TOPATH(fi->fh));
        free(FH_TOIO(fi->fh));
        FH_ZERO(fi->fh);
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
                closedir(dp);
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
                while (op->rd_req == RD_IDLE) {
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

                if (req == RD_TERM)
                        goto out;
                printd(4, "Reader thread wakeup (fp:%p)\n", op->fp);
                if (req != RD_SYNC_NOREAD && !feof(op->fp))
                        (void)iob_write(op->buf, op->fp, IOB_SAVE_HIST);
                pthread_mutex_lock(&op->rd_req_mutex);
                op->rd_req = RD_IDLE;
                pthread_cond_signal(&op->rd_req_cond); /* sync */
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
static int preload_index(struct iob *buf, const char *path)
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
        if (fd == -1)
                return -1;

#ifdef HAVE_MMAP
        /* Map the file into address space (1st pass) */
        struct idx_head *h = (struct idx_head *)mmap(NULL,
                        sizeof(struct idx_head), PROT_READ, MAP_SHARED, fd, 0);
        if (h == MAP_FAILED || h->magic != R2I_MAGIC) {
                close(fd);
                return -1;
        }
        if (ntohs(h->version) == 0) {
                syslog(LOG_INFO, "preloaded index header version 0 not supported");
                munmap((void *)h, sizeof(struct idx_head));
                close(fd);
                return -1;
        }

        /* Map the file into address space (2nd pass) */
        buf->idx.data_p = (void *)mmap(NULL, P_ALIGN_(ntoh64(h->size)),
                                               PROT_READ, MAP_SHARED, fd, 0);
        munmap((void *)h, sizeof(struct idx_head));
        if (buf->idx.data_p == MAP_FAILED) {
                close(fd);
                return -1;
        }
        buf->idx.mmap = 1;
#else
        buf->idx.data_p = malloc(sizeof(struct idx_data));
        if (!buf->idx.data_p) {
                close(fd);
                buf->idx.data_p = MAP_FAILED;
                return -1;
        }
        NO_UNUSED_RESULT read(fd, buf->idx.data_p, sizeof(struct idx_head));
        if (ntohs(buf->idx.data_p->head.version) == 0) {
                syslog(LOG_INFO, "preloaded index header version 0 not supported");
                close(fd);
                return -1;
        }
        buf->idx.mmap = 0;
#endif
        buf->idx.fd = fd;
        return 0;
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

        /* When using WinFSP/cygfuse the O_CREAT and O_EXCL flags sometimes
         * seems to be added to fi->flags in the open callback function.
         * This is not according to the legacy FUSE API implementation for
         * which these flags bits are never set. As a workaround, make sure
         * to clear flag bits that are not expected in calls to open. */
#ifdef FSP_FUSE_API
        fi->flags &= ~(O_CREAT | O_EXCL);
#endif
        errno = 0;
        pthread_rwlock_rdlock(&file_access_lock);
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
                                        pthread_rwlock_unlock(&file_access_lock);
                                        return -EIO;
                                }
                                break;
                        }
                        ++cmd;
                }
#endif
                if (entry_p == NULL) {
                        pthread_rwlock_unlock(&file_access_lock);
                        return -ENOENT;
                }
                struct io_handle *io = malloc(sizeof(struct io_handle));
                if (!io) {
                        pthread_rwlock_unlock(&file_access_lock);
                        return -EIO;
                }

                struct filecache_entry *e_p = filecache_clone(entry_p);
                pthread_rwlock_unlock(&file_access_lock);
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
                pthread_rwlock_unlock(&file_access_lock);
                ABS_ROOT(root, path);
                return lopen(root, fi);
        }
        /*
         * For files inside RAR archives open for exclusive write access
         * is not permitted. That implicity includes also O_TRUNC.
         * O_CREAT/O_EXCL is never passed to open() by FUSE so no need to
         * check those.
         */
        if (fi->flags & (O_WRONLY | O_TRUNC)) {
                pthread_rwlock_unlock(&file_access_lock);
                return -EPERM;
        }

        FILE *fp = NULL;
        struct iob *buf = NULL;
        struct io_context *op = NULL;
        struct io_handle* io = NULL;
        pid_t pid = 0;

        if (!FH_ISSET(fi->fh)) {
                if (entry_p->flags.raw) {
                        fp = fopen(entry_p->rar_p, "r");
                        if (fp != NULL) {
                                io = malloc(sizeof(struct io_handle));
                                op = calloc(1, sizeof(struct io_context));
                                if (!op || !io)
                                        goto open_error;
                                printd(3, "Opened %s\n", entry_p->rar_p);
                                FH_SETIO(fi->fh, io);
                                FH_SETTYPE(fi->fh, IO_TYPE_RAW);
                                FH_SETCONTEXT(fi->fh, op);
                                printd(3, "(%05d) %-8s%s [%-16p]\n", getpid(), "ALLOC", path, FH_TOCONTEXT(fi->fh));
                                pthread_mutex_init(&op->raw_read_mutex, NULL);
                                op->fp = fp;
                                op->pid = 0;
                                op->seq = 0;
                                op->buf = NULL;
                                op->entry_p = NULL;
                                op->pos = 0;
                                op->vno = -1;   /* force a miss 1:st time */

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

                buf = iob_alloc(P_ALIGN_(sizeof(struct iob) + IOB_SZ));
                if (!buf)
                        goto open_error;

                io = malloc(sizeof(struct io_handle));
                op = calloc(1, sizeof(struct io_context));
                if (!op || !io)
                        goto open_error;
                op->buf = buf;
                op->entry_p = NULL;

                /* Open PIPE(s) and create child process */
                fp = popen_(entry_p, &pid);
                if (fp != NULL) {
                        FH_SETIO(fi->fh, io);
                        FH_SETTYPE(fi->fh, IO_TYPE_RAR);
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

                        /* Promote to a write lock since we might need to
                         * change the cache entry below. */
			pthread_rwlock_unlock(&file_access_lock);
			pthread_rwlock_wrlock(&file_access_lock);

                        buf->idx.data_p = MAP_FAILED;
                        buf->idx.fd = -1;
                        if (!preload_index(buf, path)) {
                                entry_p->flags.save_eof = 0;
                                entry_p->flags.direct_io = 0;
                                fi->direct_io = 0;
                        } else {
                                /* Was the file removed ? */
                                if (get_save_eof(entry_p->rar_p) && !entry_p->flags.save_eof) {
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
        pthread_rwlock_unlock(&file_access_lock);
        if (fp) {
                if (entry_p->flags.raw)
                        fclose(fp);
                else
                        pclose_(fp, pid);
        }
	free(io);
        if (op) {
                if (op->entry_p)
                        filecache_freeclone(op->entry_p);
                free(op);
        }
        iob_free(buf);

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
        pthread_rwlock_unlock(&file_access_lock);
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
         *   This works fine in most cases but it does not always work for
         * some specific programs like 'touch'. A 'touch' may result in a
         * getattr() callback even if -EPERM is returned by open(), mknod()
         ' etc. This will eventually render a "No such file or directory"
         * type of error/message.
         */
        if (new_file) {
                char *p = strdup(path); /* In case p is destroyed by dirname() */
                e = filecache_get(__gnu_dirname(p));
                free(p);
        } else {
                e = filecache_get(path);
        }
        return e && !e->flags.unresolved ? 1 : 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *walkpath_task(void *data)
{
        pthread_detach(pthread_self());

        syncdir(data);
        free(data);

        pthread_mutex_lock(&warmup_lock);
        --warmup_threads;
        pthread_cond_broadcast(&warmup_cond);
        pthread_mutex_unlock(&warmup_lock);

        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void walkpath(const char *dname, size_t src_len)
{
        pthread_t t;
        struct dirent *dent;
        DIR *dir = NULL;
        char *fn = NULL;
        struct stat st;
        int len;
        const char *root = &dname[src_len];

        if (*root == '\0')
              root = "/";
        root = strdup(root);
        if (root == NULL)
                goto out;

        pthread_mutex_lock(&warmup_lock);
        while (warmup_threads > (rar2fs_mount_opts.warmup - 1))
                pthread_cond_wait(&warmup_cond, &warmup_lock);
        ++warmup_threads;
        pthread_mutex_unlock(&warmup_lock);
        if (pthread_create(&t, NULL, walkpath_task, (void *)root)) {
                free((void *)root);
                goto out;
        }

        len = strlen(dname);
        if (len >= FILENAME_MAX - 1)
                return;

        dir = opendir(dname);
        if (dir == NULL)
                goto out;

        /* From www.gnu.org:
         *   Macro: int FILENAME_MAX
         *     The value of this macro is an integer constant expression that
         *     represents the maximum length of a file name string. It is
         *     defined in stdio.h.
         *
         *     Unlike PATH_MAX, this macro is defined even if there is no actual
         *     limit imposed. In such a case, its value is typically a very
         *     large number. This is always the case on GNU/Hurd systems.
         *
         *     Usage Note: Don't use FILENAME_MAX as the size of an array in
         *     which to store a file name! You can't possibly make an array that
         *     big! Use dynamic allocation instead.
         */
        fn = malloc(FILENAME_MAX);
        if (fn == NULL)
                goto out;

        strcpy(fn, dname);
        fn[len++] = '/';

        /* Do not use reentrant version of readdir(3) here.
         * This needs to be revisted if other threads starts to use it. */
        while ((dent = readdir(dir))) {
                if (warmup_cancelled)
                        break;
                /* Skip '.' and '..' */
                if (dent->d_name[0] == '.') {
                        if (dent->d_name[1] == 0 ||
                                        (dent->d_name[1] == '.' &&
                                        dent->d_name[2] == 0))
                                continue;
                }

                strncpy(fn + len, dent->d_name, FILENAME_MAX - len);
#ifdef _DIRENT_HAVE_D_TYPE
                if (dent->d_type != DT_UNKNOWN) {
                        if (dent->d_type == DT_DIR)
                                walkpath(fn, src_len);
                        continue;
                }
#endif
                if (lstat(fn, &st) == -1)
                        continue;
                /* will be false for symlinked dirs */
                if (S_ISDIR(st.st_mode))
                        walkpath(fn, src_len);
        }

out:
        if (fn)
                free(fn);
        if (dir)
                closedir(dir);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *warmup_task(void *data)
{
        (void)data;
        const char *dir = OPT_STR(OPT_KEY_SRC, 0);
        struct timeval t1;
        struct timeval t2;

        pthread_detach(pthread_self());

        syslog(LOG_DEBUG, "cache warmup started");
        gettimeofday(&t1, NULL);

        walkpath(dir, strlen(dir));

        pthread_mutex_lock(&warmup_lock);
        while (warmup_threads)
                pthread_cond_wait(&warmup_cond, &warmup_lock);
        pthread_mutex_unlock(&warmup_lock);

        if (!warmup_cancelled) {
                gettimeofday(&t2, NULL);
                syslog(LOG_DEBUG, "cache warmup completed after %d seconds",
                       (int)(t2.tv_sec - t1.tv_sec));
        }

        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __dircache_free(const char *path, struct dir_entry_list *dir)
{
        struct dir_entry_list *next;

        pthread_rwlock_wrlock(&file_access_lock);
        if (dir) {
                next = dir->next;
                while (next) {
                        char *mp;
                        ABS_MP2(mp, path, next->entry.name);
                        filecache_invalidate(mp);
                        next = next->next;
                        free(mp);
                }
        }
        filecache_invalidate(path);
        pthread_rwlock_unlock(&file_access_lock);

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __dircache_stale(const char *path)
{
        /* Promote to a wrlock (might already be) */
        pthread_rwlock_unlock(&dir_access_lock);
        pthread_rwlock_wrlock(&dir_access_lock);
        dircache_invalidate(path);

        return 0;
}

static struct dircache_cb dircache_cb = {
        .stale = __dircache_stale,
        .free = __dircache_free,
};

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *rar2_init(struct fuse_conn_info *conn)
{
        ENTER_();

        pthread_t t;
        (void)conn;             /* touch */

        filecache_init();
        dircache_init(&dircache_cb);
        iob_init();
        sighandler_init();
        if (mount_type == MOUNT_FOLDER && rar2fs_mount_opts.warmup > 0)
                pthread_create(&t, NULL, warmup_task, NULL);

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

        if (mount_type == MOUNT_FOLDER && rar2fs_mount_opts.warmup > 0) {
                pthread_mutex_lock(&warmup_lock);
                if (warmup_threads)
                        printf("shutting down...\n");
                while (warmup_threads)
                        pthread_cond_wait(&warmup_cond, &warmup_lock);
                pthread_mutex_unlock(&warmup_lock);
        }

        iob_destroy();
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

        pthread_rwlock_rdlock(&file_access_lock);
        entry_p = path_lookup(path, NULL);
        if (entry_p && entry_p != LOCAL_FS_ENTRY) {
                if (entry_p->link_target_p) {
                        strncpy(buf, entry_p->link_target_p, buflen - 1);
                        pthread_rwlock_unlock(&file_access_lock);
                } else {
                        pthread_rwlock_unlock(&file_access_lock);
                        return -EIO;
                }
        } else {
                pthread_rwlock_unlock(&file_access_lock);
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
                if (op->fp && op->entry_p->flags.raw) {
                        printd(3, "Closing file handle %p\n", op->fp);
                        fclose(op->fp);
                        pthread_mutex_destroy(&op->raw_read_mutex);
                }
                printd(3, "(%05d) %s [0x%-16" PRIx64 "]\n", getpid(), "FREE", fi->fh);
                if (op->buf) {
                        __wake_thread(op, RD_TERM);
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

#ifdef HAVE_MMAP
                        if (op->buf->idx.data_p != MAP_FAILED &&
                                        op->buf->idx.mmap)
                                munmap((void *)op->buf->idx.data_p,
                                       P_ALIGN_(ntoh64(op->buf->idx.data_p->head.size)));
#endif
                        if (op->buf->idx.data_p != MAP_FAILED &&
                                        !op->buf->idx.mmap)
                                free(op->buf->idx.data_p);
                        if (op->buf->idx.fd != -1)
                                close(op->buf->idx.fd);
                        iob_free(op->buf);
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
static int rar2_eperm()
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
                if (!mknod(root, mode, dev)) {
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
                        __dircache_invalidate(path);
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

        struct stat st;

        if (!access_chk(path, 0)) {
                int res;
                char *root;
                ABS_ROOT(root, path);
                /* don't use utime/utimes since they follow symlinks */
                res = utimensat(0, root, ts, AT_SYMLINK_NOFOLLOW);
                if (res == -1)
                        return -errno;
                /* A directory cache invalidation is not really necessary for
                 * regular files, but checking if it is is possibly even less
                 * effective than the extra invalidation itself. For now do
                 * the check for a directory. */
                if (!stat(root, &st) && S_ISDIR(st.st_mode))
                        __dircache_invalidate(path);
                return 0;
        }
        return -EPERM;
}
#endif

#ifdef HAVE_SETXATTR

static const char *xattr[4] = {
        "user.rar2fs.cache_method",
        "user.rar2fs.cache_flags",
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
                return -ENOENT;

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
                 * -ENODATA if ENOATTR is not defined.
                 */
#ifdef HAVE_ENOATTR
                return -ENOATTR;
#else
                return -ENODATA;
#endif
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
                return -ENOENT;

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
        int res;

        ENTER_("%s", path);

        if (!access_chk(path, 0)) {
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

#ifndef __CYGWIN__
/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int parse_fuse_fd(const char *mountpoint)
{
        int fd = -1;
        unsigned int len = 0;

        if (sscanf(mountpoint, "/dev/fd/%u%n", &fd, &len) == 1 &&
            len == strlen(mountpoint))
                return fd;

        return -1;
}
#endif

/*!
 *****************************************************************************
 * Converts the given ERAR error code into a matching string.
 * The returned string is statically allocated and does not need to be freed.
 ****************************************************************************/
static const char *error_to_string(int err)
{
        switch (err) {
#define ERROR_TO_STRING_ENTRY(s) \
        case s:                  \
                return #s;
                ERROR_TO_STRING_ENTRY(ERAR_SUCCESS);
                ERROR_TO_STRING_ENTRY(ERAR_END_ARCHIVE);
                ERROR_TO_STRING_ENTRY(ERAR_NO_MEMORY);
                ERROR_TO_STRING_ENTRY(ERAR_BAD_DATA);
                ERROR_TO_STRING_ENTRY(ERAR_BAD_ARCHIVE);
                ERROR_TO_STRING_ENTRY(ERAR_UNKNOWN_FORMAT);
                ERROR_TO_STRING_ENTRY(ERAR_EOPEN);
                ERROR_TO_STRING_ENTRY(ERAR_ECREATE);
                ERROR_TO_STRING_ENTRY(ERAR_ECLOSE);
                ERROR_TO_STRING_ENTRY(ERAR_EREAD);
                ERROR_TO_STRING_ENTRY(ERAR_EWRITE);
                ERROR_TO_STRING_ENTRY(ERAR_SMALL_BUF);
                ERROR_TO_STRING_ENTRY(ERAR_UNKNOWN);
                ERROR_TO_STRING_ENTRY(ERAR_MISSING_PASSWORD);
#ifdef ERAR_EREFERENCE
                ERROR_TO_STRING_ENTRY(ERAR_EREFERENCE);
#endif
#ifdef ERAR_BAD_PASSWORD
                ERROR_TO_STRING_ENTRY(ERAR_BAD_PASSWORD);
#endif
#undef ERROR_TO_STRING_ENTRY
        }

        return "Unexpected ERAR code";
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int check_paths(const char *prog, char *src_path, char *dst_path)
{
        struct stat st;
        char *src_path_out;
        char *dst_path_out;

        char *a1 = realpath(src_path, NULL);

        if (!a1) {
                printf("%s: invalid source: %s\n", prog, src_path);
                return -1;
        }

#ifdef __CYGWIN__
        char *a2 = dst_path;
#else
        char *a2;

        /* Check if destination path is a pre-mounted FUSE descriptor. */
        const int fuse_fd = parse_fuse_fd(dst_path);
        if (fuse_fd >= 0) {
                a2 = strdup(dst_path);
        } else {
                a2 = realpath(dst_path, NULL);

                if (!a2) {
                        printf("%s: invalid mount point: %s\n", prog,
                               dst_path);
                        return -1;
                }

                /* Check if destination path is a directory. */
                (void)stat(a2, &st);
                if (!S_ISDIR(st.st_mode)) {
                        printf("%s: mount point '%s' is not a directory\n",
                               prog, a2);
                        return -1;
                }
        }
#endif

        if (!strcmp(a1, a2)) {
                printf("%s: source and mount point are the same: %s\n",
                       prog, a1);
                return -1;
        }

        (void)stat(a1, &st);
        mount_type = S_ISDIR(st.st_mode) ? MOUNT_FOLDER : MOUNT_ARCHIVE;

        /* Check for block special file */
        if (mount_type == MOUNT_ARCHIVE && S_ISBLK(st.st_mode))
                blkdev_size = get_blkdev_size(&st);

#ifndef __CYGWIN__
        char *tmp = a2;
#endif
        src_path_full = strdup(a1);
        /* Do not try to use 'a1' after this call since dirname() will destroy it! */
        src_path_out = mount_type == MOUNT_FOLDER
                ? a1 : __gnu_dirname(a1);
        dst_path_out = a2;
        optdb_save(OPT_KEY_SRC, src_path_out);
        optdb_save(OPT_KEY_DST, dst_path_out);
        free(a1);
#ifndef __CYGWIN__
        free(tmp);
#endif
        src_path_out = OPT_STR(OPT_KEY_SRC, 0);
        dst_path_out = OPT_STR(OPT_KEY_DST, 0);

        /* Detect a possible file system loop */
        if (mount_type == MOUNT_FOLDER) {
                if (!strncmp(src_path_out, dst_path_out,
                                        strlen(src_path_out))) {
                        if ((dst_path_out)[strlen(src_path_out)] == '/') {
                                memcpy(&fs_loop_mp_stat, &st, sizeof(struct stat));
                                fs_loop = 1;
                                char *safe_path = strdup(dst_path);
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
                rar2_operations.opendir         = rar2_opendir;
                rar2_operations.readdir         = rar2_readdir;
                rar2_operations.releasedir      = rar2_releasedir;
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
                rar2_operations.opendir         = rar2_opendir2;
                rar2_operations.readdir         = rar2_readdir2;
                rar2_operations.releasedir      = rar2_releasedir2;
                rar2_operations.create          = (void *)rar2_eperm;
                rar2_operations.rename          = (void *)rar2_eperm;
                rar2_operations.mknod           = (void *)rar2_eperm;
                rar2_operations.unlink          = (void *)rar2_eperm;
                rar2_operations.mkdir           = (void *)rar2_eperm;
                rar2_operations.rmdir           = (void *)rar2_eperm;
                rar2_operations.write           = (void *)rar2_eperm;
                rar2_operations.truncate        = (void *)rar2_eperm;
                rar2_operations.chmod           = (void *)rar2_eperm;
                rar2_operations.chown           = (void *)rar2_eperm;
                rar2_operations.symlink         = (void *)rar2_eperm;
        }

        struct fuse *f = NULL;
        struct fuse_chan *ch = NULL;
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
                              fuse_daemonize(fg);
                              fuse_set_signal_handlers(fuse_get_session(f));
                              syslog(LOG_DEBUG, "mounted %s", mp);
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
        warmup_cancelled = 1;
        pthread_join(t, NULL);

        /* This is doing more or less the same as fuse_teardown(). */
        fuse_remove_signal_handlers(fuse_get_session(f));
        fuse_unmount(mp, ch);
        fuse_destroy(f);
        syslog(LOG_DEBUG, "unmounted %s", mp);
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
        printf("    --date-rar\t\t    use file date from main archive file(s)\n");
        printf("    --config=file\t    config file name [source/.rarconfig]\n");
        printf("    --no-inherit-perm\t    do not inherit file permission mode from archive\n");
        printf("\n");
#ifdef HAVE_SETLOCALE
        printf("    -o locale=LOCALE        set the locale for file names (default: according to LC_*/LC_CTYPE)\n");
#endif
        printf("    -o warmup[=THREADS]     start background cache warmup threads (default: 5)\n");
}

/* FUSE API specific keys continue where 'optdb' left off */
enum {
        OPT_KEY_HELP = OPT_KEY_END,
        OPT_KEY_VERSION,
};

static struct fuse_opt rar2fs_opts[] = {
#ifdef HAVE_SETLOCALE
        RAR2FS_MOUNT_OPT("locale=%s", locale, 0),
#endif
        RAR2FS_MOUNT_OPT("warmup=%d", warmup, 0),
        RAR2FS_MOUNT_OPT("warmup", warmup, 5),

        FUSE_OPT_KEY("-V",              OPT_KEY_VERSION),
        FUSE_OPT_KEY("--version",       OPT_KEY_VERSION),
        FUSE_OPT_KEY("-h",              OPT_KEY_HELP),
        FUSE_OPT_KEY("--help",          OPT_KEY_HELP),
/* Allow --VolumePrefix=UNC option to be passed to WinFSP */
#ifdef FSP_FUSE_API
        FUSE_OPT_KEY("--VolumePrefix=", FUSE_OPT_KEY_KEEP),
#endif
        FUSE_OPT_END
};

static struct option longopts[] = {
        {"exclude",     required_argument, NULL, OPT_ADDR(OPT_KEY_EXCLUDE)},
        {"seek-length", required_argument, NULL, OPT_ADDR(OPT_KEY_SEEK_LENGTH)},
#if defined ( HAVE_SCHED_SETAFFINITY ) && defined ( HAVE_CPU_SET_T )
        {"no-smp",            no_argument, NULL, OPT_ADDR(OPT_KEY_NO_SMP)},
#endif
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
        {"date-rar",          no_argument, NULL, OPT_ADDR(OPT_KEY_DATE_RAR)},
        {"config",      required_argument, NULL, OPT_ADDR(OPT_KEY_CONFIG)},
        {"no-inherit-perm",   no_argument, NULL, OPT_ADDR(OPT_KEY_NO_INHERIT_PERM)},
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

#ifdef HAVE_UMASK
        umask_ = umask(0);
        umask(umask_);
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
        if (fuse_opt_parse(&args, &rar2fs_mount_opts, rar2fs_opts,
                           rar2fs_opt_proc))
                return -1;

        /* Check src/dst path */
        if (OPT_SET(OPT_KEY_SRC) && OPT_SET(OPT_KEY_DST)) {
                const int err = check_paths(argv[0], OPT_STR(OPT_KEY_SRC, 0),
                                            OPT_STR(OPT_KEY_DST, 0));
                if (err)
                        return err;
        } else {
                usage(argv[0]);
                return 0;
        }

        /* This must be initialized before a call to collect_files() */
        rarconfig_init(OPT_STR(OPT_KEY_SRC, 0),
                       OPT_STR(OPT_KEY_CONFIG, 0));

        /* Check file collection at archive mount */
        if (mount_type == MOUNT_ARCHIVE) {
                const int ret = collect_files(src_path_full);
                if (ret < 0) {
                        const int err = -ret;
                        printf("%s: cannot open '%s': %s\n", argv[0],
                               src_path_full, error_to_string(err));
                        return err;
                }
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

        /* Set locale.
         * Default to environment variables according to LC_* and LANG;
         * see the Base Definitions volume of POSIX.1-2008, Chapter 7,
         * Locale and Chapter 8, Environment Variables. */
#ifdef HAVE_SETLOCALE
        if (!rar2fs_mount_opts.locale) {
                setlocale(LC_CTYPE, "");
        } else {
                if (!setlocale(LC_CTYPE, rar2fs_mount_opts.locale)) {
                        printf("%s: failed to set locale: %s\n", argv[0],
                               rar2fs_mount_opts.locale);
                        return -1;
                }
        }
#endif

        /*
         * All static setup is ready, the rest is taken from the configuration.
         * Continue in work() function which will not return until the process
         * is about to terminate.
         */
        res = work(&args);

        /* Clean up what has not already been taken care of */
        fuse_opt_free_args(&args);
        rarconfig_destroy();
        optdb_destroy();
        if (mount_type == MOUNT_ARCHIVE)
                dir_list_free(arch_list);
        if (fs_loop) {
                free(fs_loop_mp_root);
                free(fs_loop_mp_base);
        }
        free(src_path_full);

        closelog();

        return res;
}
