
/*
    Copyright (C) 2009 Hans Beckerus (hans.beckerus@gmail.com)

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

#include "platform.h"
#include <string.h>
#include <stdlib.h>
#include "debug.h"
#include "hashtable.h"
#include "hash.h"

struct hash_table {
        struct hash_table_entry *bucket;
        size_t size;
        struct hash_table_ops ops;
};

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct hash_table_entry *hashtable_entry_alloc(void *h, const char *key)
{
        return hashtable_entry_alloc_hash(h, key, get_hash(key, 0));
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct hash_table_entry *hashtable_entry_alloc_hash(void *h, const char *key, uint32_t hash)
{
        struct hash_table *ht = h;
        struct hash_table_entry *p = &ht->bucket[(hash & (ht->size - 1))];
        if (p->key) {
                if (!strcmp(key, p->key))
                        return p;
                while (p->next) {
                        p = p->next;
                        if (hash == p->hash && !strcmp(key, p->key))
                                return p;
                }
                p->next = malloc(sizeof(struct hash_table_entry));
                p = p->next;
                if (p) {
                        p->next = NULL;
                        p->key = strdup(key);
                        p->hash = hash;
                        p->user_data = ht->ops.alloc();
                        return p;
                }
                return NULL;
        }
        p->key = strdup(key);
        p->hash = hash;
        p->user_data = ht->ops.alloc();
        return p;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct hash_table_entry *hashtable_entry_get(void *h, const char *key)
{
        return hashtable_entry_get_hash(h, key, get_hash(key, 0));
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
struct hash_table_entry *hashtable_entry_get_hash(void *h, const char *key, uint32_t hash)
{
        struct hash_table *ht = h;
        struct hash_table_entry *p = &ht->bucket[hash & (ht->size - 1)];
        if (p->key) {
                while (p) {
                        /*
                         * Checking the full hash here will inflict a small
                         * cache hit penalty for the bucket but will
                         * instead improve speed when searching a collision
                         * chain due to less calls needed to strcmp().
                         */
                        if (hash == p->hash && !strcmp(key, p->key))
                                return p;
                        p = p->next;
                }
        }
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void hashtable_entry_delete_hash(void *h, const char *key,
                                        uint32_t hash)
{
        struct hash_table *ht = h;
        struct hash_table_entry *b = &ht->bucket[hash & (ht->size - 1)];
        struct hash_table_entry *p = b;
        printd(3, "Invalidating hash key %s in %p\n", key, ht);

        /* Search collision chain */
        while (p->next) {
                struct hash_table_entry *prev = p;
                p = p->next;
                if (p->key && !strcmp(key, p->key)) {
                        if (p->user_data)
                                ht->ops.free(p->user_data);
                        prev->next = p->next;
                        free(p->key);
                        free(p);
                        /* Entry purged. We can leave now. */
                        return;
                }
        }

        /*
         * Entry not found in collision chain.
         * Most likely it is in the bucket, but double check.
         */
        if (b->key && !strcmp(b->key, key)) {
                /* Need to relink collision chain */
                if (b->next) {
                        struct hash_table_entry *tmp = b->next;
                        if (b->user_data)
                                ht->ops.free(b->user_data);
                        free(b->key);
                        memcpy(b, b->next,
                                    sizeof(struct hash_table_entry));
                        free(tmp);
                } else {
                        ht->ops.free(b->user_data);
                        free(b->key);
                        memset(b, 0, sizeof(struct hash_table_entry));
                }
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void hashtable_entry_delete(void *h, const char *key)
{
        struct hash_table *ht = h;
        size_t i;

        if (key) {
                hashtable_entry_delete_hash(h, key, get_hash(key, 0));
        } else {
                printd(3, "Invalidating all hash keys in %p\n", ht);
                for (i = 0; i < ht->size; i++) {
                        struct hash_table_entry *b = &ht->bucket[i];
                        struct hash_table_entry *next = b->next;

                        /* Search collision chain */
                        while (next) {
                                struct hash_table_entry *p = next;
                                next = p->next;
                                if (p->user_data)
                                        ht->ops.free(p->user_data);
                                free(p->key);
                                free(p);
                        }
                        if (b->user_data)
                                ht->ops.free(b->user_data);
                        free(b->key);
                        memset(b, 0, sizeof(struct hash_table_entry));
                }
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void hashtable_entry_delete_subkeys(void *h, const char *key, uint32_t hash)
{
        struct hash_table *ht = h;
        struct hash_table_entry *p;
        struct hash_table_entry *b;

/* Use a linear search algorithm, at least until we can make sure that
 * parent and child nodes are all populated in the cache together, i.e. no
 * broken chains. */
#if 0
        p = &ht->bucket[hash & (ht->size - 1)];
        b = p;

        /* Search collision chain first */
        while (p->next) {
                p = p->next;
                if (p->key && (hash == p->hash) &&
                    (strstr(p->key, key) == p->key)) {
                        hashtable_entry_delete_subkeys(h, key,
                                                       get_hash(p->key, 0));
                        hashtable_entry_delete_hash(h, p->key, hash);
                }
        }
        /* Finally check the bucket */
        if (b->key && (hash == b->hash) &&
            (strstr(b->key, key) == b->key)) {
                        hashtable_entry_delete_subkeys(h, key,
                                                       get_hash(b->key, 0));
                        hashtable_entry_delete_hash(h, b->key, hash);
        }
#else
        size_t i;
        (void)hash;

        for (i = 0; i < ht->size; i++) {
                b = &ht->bucket[i];
                p = b;
                while (p->next) {
                        p = p->next;
                        if (strstr(p->key, key) == p->key) {
                                hashtable_entry_delete_hash(h, p->key, p->hash);
                                p = b;
                        }
                }
                if (b->key && (strstr(b->key, key) == b->key))
                        hashtable_entry_delete_hash(h, b->key, b->hash);
        }
#endif
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void *hashtable_init(size_t size, struct hash_table_ops *ops)
{
        struct hash_table *ht;
        uint32_t mul;

        /* Size must be a pow(2, n) multiple of 1024 */
        mul = (size + 1023) / 1024;
        if (mul & (mul - 1)) {
                uint32_t pow = 1;
                while (pow < size)
                        pow *= 2;
                size = 1024 << (pow + 1);
        } else {
                size = mul * 1024;
        }

        ht = malloc(sizeof(struct hash_table));
        if (ht) {
                memset(ht, 0, sizeof(struct hash_table));
                ht->bucket = malloc(size * sizeof(struct hash_table_entry));
                if (!ht->bucket) {
                        free(ht);
                        return NULL;
                }
                memset(ht->bucket, 0, size * sizeof(struct hash_table_entry));
                if (ops)
                        ht->ops = *ops;
                ht->size = size;
        }
        return ht;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void hashtable_destroy(void *h)
{
        struct hash_table *ht = h;
        hashtable_entry_delete(ht, NULL);
        free(ht->bucket);
        free(ht);
}
