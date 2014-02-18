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
#include <memory.h>
#include <pthread.h>
#include "debug.h"
#include "iobuffer.h"
#include "optdb.h"

size_t iob_hist_sz = 0;
size_t iob_sz = 0;

static pthread_mutex_t io_mutex = PTHREAD_MUTEX_INITIALIZER;

#define SPACE_LEFT(ri, wi) (IOB_SZ - SPACE_USED((ri), (wi)))
#define SPACE_USED(ri, wi) (((wi) - (ri)) & (IOB_SZ-1))

/*!
 *****************************************************************************
 *
 ****************************************************************************/
size_t readTo(struct io_buf *dest, FILE *fp, int hist)
{
        unsigned tot = 0;
        pthread_mutex_lock(&io_mutex);
        unsigned int lwi = dest->wi;  /* read once */
        unsigned int lri = dest->ri;  
        pthread_mutex_unlock(&io_mutex);
        size_t left = SPACE_LEFT(lri, lwi) - 1;   /* -1 to avoid wi = ri */
        if (IOB_HIST_SZ && hist == IOB_SAVE_HIST) {
                left = left > IOB_HIST_SZ ? left - IOB_HIST_SZ : 0;
                if (!left) {
                        return 0; /* quick exit */
                }
        }
        unsigned int chunk = IOB_SZ - lwi;   /* assume one large chunk */
        chunk = chunk < left ? chunk : left; /* reconsider assumption */
        while (left > 0) {
                size_t n = fread(dest->data_p + lwi, 1, chunk, fp);
                if (n != chunk) {
                        if (ferror(fp)) {
                                perror("read");
                                break;
                        }
                        if (!n)
                                break;
                }
                left -= n;
                lwi = (lwi + n) & (IOB_SZ - 1);
                tot += n;
                chunk -= n;
                chunk = !chunk ? left : chunk;
        }
        pthread_mutex_lock(&io_mutex);
        dest->wi = lwi;
        dest->used = SPACE_USED(dest->ri, lwi); /* dest->ri might have changed */
        pthread_mutex_unlock(&io_mutex);
        MB();
        dest->offset += tot;

        return tot;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
size_t readFrom(char *dest, struct io_buf *src, size_t size, size_t off)
{
        size_t tot = 0;
        pthread_mutex_lock(&io_mutex);
        unsigned int lri = src->ri; /* read once */
        size_t used = src->used;
        pthread_mutex_unlock(&io_mutex);
        if (off) {
                /* consume offset */
                off = off < used ? off : used;
                lri = (lri + off) & (IOB_SZ - 1);
                used -= off;
        }
        size = size > used ? used : size;    /* can not read more than used */
        unsigned int chunk = IOB_SZ - lri;   /* assume one large chunk */
        chunk = chunk < size ? chunk : size; /* reconsider assumption */
        while (size) {
                memcpy(dest, src->data_p + lri, chunk);
                lri = (lri + chunk) & (IOB_SZ - 1);
                tot += chunk;
                size -= chunk;
                dest += chunk;
                chunk = size;
        }
        pthread_mutex_lock(&io_mutex);
        src->ri = lri;
        src->used -= tot;       /* src->used might have changed */
        pthread_mutex_unlock(&io_mutex);

        return tot;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
size_t copyFrom(char *dest, struct io_buf *src, size_t size, size_t pos)
{
        size_t tot = 0;
        unsigned int chunk = IOB_SZ - pos;   /* assume one large chunk */
        chunk = chunk < size ? chunk : size; /* reconsider assumption */
        pthread_mutex_lock(&io_mutex);
        while (size) {
                memcpy(dest, src->data_p + pos, chunk);
                pos = (pos + chunk) & (IOB_SZ - 1);
                tot += chunk;
                size -= chunk;
                dest += chunk;
                chunk = size;
        }
        pthread_mutex_unlock(&io_mutex);
        return tot;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void iobuffer_init()
{
        int bsz = OPT_INT(OPT_KEY_BUF_SIZE,0);
        iob_sz = bsz ? (bsz * 1024 * 1024) : IOB_SZ_DEFAULT;
        int hsz = OPT_SET(OPT_KEY_HIST_SIZE) ? OPT_INT(OPT_KEY_HIST_SIZE, 0) : 50;
        iob_hist_sz = IOB_SZ * (hsz / 100.0);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void iobuffer_destroy()
{
}

