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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#include <libgen.h>
#include <pthread.h>
#include "hashtable.h"
#include "version.hpp"
#include "rarconfig.h"
#include "dirname.h"

#define RARCONFIG_SZ 1024

struct parent_node {
        off_t pos;
        char *name;
};

struct child_node {
        off_t pos;
        char *name;
        char *value;
};

struct alias_entry {
        char *file;
        char *alias;
};

struct config_entry {
        int seek_length;
        int save_eof;
        wchar_t *password_w;
        char *password;
        struct alias_entry *aliases;
        size_t n_aliases;
        uint32_t mask;
};

/* Hash table handle */
static void *ht = NULL;
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;

/*!
 *****************************************************************************
 *
 ****************************************************************************/
int rarconfig_getprop_int(const char *path, int prop)
{
        struct hash_table_entry *hte;

        pthread_mutex_lock(&config_mutex);
        hte = ht ? hashtable_entry_get(ht, path) : NULL;
        if (hte) {
                struct config_entry *e = hte->user_data;
                switch (prop) {
                case RAR_SEEK_LENGTH_PROP:
                        pthread_mutex_unlock(&config_mutex);
                        return e->mask & RAR_SEEK_LENGTH_PROP
                                        ? e->seek_length : -1;
                case RAR_SAVE_EOF_PROP:
                        pthread_mutex_unlock(&config_mutex);
                        return e->mask & RAR_SAVE_EOF_PROP
                                        ? e->save_eof : -1;
                }
        }
        pthread_mutex_unlock(&config_mutex);
        return -1;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
const char *rarconfig_getprop_char(const char *path, int prop)
{
        struct hash_table_entry *hte;

        pthread_mutex_lock(&config_mutex);
        hte = ht ? hashtable_entry_get(ht, path) : NULL;
        if (hte) {
                struct config_entry *e = hte->user_data;
                switch (prop) {
                case RAR_PASSWORD_PROP:
                        pthread_mutex_unlock(&config_mutex);
                        return e->mask & RAR_PASSWORD_PROP
                                ? e->password : NULL;
                }
        }
        pthread_mutex_unlock(&config_mutex);
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
const wchar_t *rarconfig_getprop_wchar(const char *path, int prop)
{
        struct hash_table_entry *hte;

        pthread_mutex_lock(&config_mutex);
        hte = ht ? hashtable_entry_get(ht, path) : NULL;
        if (hte) {
                struct config_entry *e = hte->user_data;
                switch (prop) {
                case RAR_PASSWORD_PROP:
                        pthread_mutex_unlock(&config_mutex);
                        return e->mask & RAR_PASSWORD_PROP
                                ? e->password_w : NULL;
                }
        }
        pthread_mutex_unlock(&config_mutex);
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
const char *rarconfig_getalias(const char *path, const char *file)
{
        struct hash_table_entry *hte;

        pthread_mutex_lock(&config_mutex);
        hte = ht ? hashtable_entry_get(ht, path) : NULL;
        if (hte) {
                size_t n;
                struct config_entry *e = hte->user_data;
                for (n = 0; n < e->n_aliases; n++) {
                        if (!strcmp(file, e->aliases[n].file)) {
                                char *alias = e->aliases[n].alias;
                                pthread_mutex_unlock(&config_mutex);
                                return alias;
                        }
                }
        }
        pthread_mutex_unlock(&config_mutex);
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
__attribute__((unused))
static void __patch_alias(struct config_entry *e, const char *file,
                const char *alias)
{
        size_t n;

        for (n = 0; n < e->n_aliases; n++) {
                struct alias_entry *ax = &e->aliases[n];
                char *sub = strstr(ax->alias, file);
                if (sub && sub == ax->alias) {
                        size_t file_sz = strlen(file);
                        if (ax->alias[file_sz] == '/') {
                                size_t sz = strlen(ax->alias) -
                                        file_sz + strlen(alias) + 1;
                                char *tmp = malloc(sz);
                                if (tmp) {
                                        strcpy(tmp, alias);
                                        strcat(tmp, ax->alias + file_sz);
                                        free(ax->alias);
                                        ax->alias = tmp;
                                }
                        }
                }
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __set_alias(struct config_entry *e, const char *file,
                const char *alias)
{
        e->aliases = realloc(e->aliases,
                sizeof(struct alias_entry) * (e->n_aliases + 1));
        e->aliases[e->n_aliases].file = strdup(file);
        e->aliases[e->n_aliases].alias = strdup(alias);
#if 0
        /* Check if any existing aliases need to be patched up */
        __patch_alias(e, file, alias);
#endif
        ++e->n_aliases;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void rarconfig_setalias(const char *path, const char *file, const char *alias)
{
        struct hash_table_entry *hte;

        pthread_mutex_lock(&config_mutex);
        hte = ht ? hashtable_entry_get(ht, path) : NULL;
        if (hte)
                __set_alias(hte->user_data, file, alias);
        pthread_mutex_unlock(&config_mutex);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static struct parent_node *find_next_parent(FILE *fp, struct parent_node *p)
{
        char s[LINE_MAX];
        char line[LINE_MAX];

        if (p)
                fseek(fp, p->pos, SEEK_SET);
        else
                fseek(fp, 0L, SEEK_SET);

        while (fgets(line, LINE_MAX, fp)) {
                const char *lstart=line;
                while (isspace(*lstart)) lstart++;
                if (*lstart!='[') continue;
                lstart++;

                const char *lend=lstart+strlen(lstart)-1;
                while (lend>lstart && *lend!=']') lend--;
                if (lend==lstart) continue;    // Shall we warn on invalid line here?
                *(char*)lend = '\0';

                p = malloc(sizeof(struct parent_node));
                p->name = strdup(lstart);
                p->pos = ftell(fp);
                return p;
        }

        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void free_parent(struct parent_node *p)
{
        if (p) {
                free(p->name);
                free(p);
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static struct child_node *find_next_child(FILE *fp, struct parent_node *p,
                        struct child_node *cnode)
{
        char s[LINE_MAX];
        char v[LINE_MAX];
        char line[LINE_MAX];

        if (!cnode)
                fseek(fp, p->pos, SEEK_SET);
        else
                fseek(fp, cnode->pos, SEEK_SET);

        while (fgets(line, LINE_MAX, fp)) {
                s[0] = 0;
                if (sscanf(line, " %[^#!=]=%[^\n]", s, v) == 2) {
                        sscanf(s, "%s", s); /* trim white-spaces */
                        cnode = malloc(sizeof(struct child_node));
                        cnode->name = strdup(s);
                        cnode->value = strdup(v);
                        cnode->pos = ftell(fp);
                        return cnode;
                } else {
                       sscanf(s, "%s", s); /* trim white-spaces */
                       if (s[0] == '[')
                               return NULL;
                }
        }

        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void free_child(struct child_node *c)
{
        if (c) {
                free(c->name);
                free(c->value);
                free(c);
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void *__alloc()
{
        struct config_entry *e;
        e = malloc(sizeof(struct config_entry));
        memset(e, 0, sizeof(struct config_entry));
        return e;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __free(const char *key, void *data)
{
        (void)key;

        struct config_entry *e = data;
        free(e->password);
        free(e->password_w);
        while (e->n_aliases) {
                --e->n_aliases;
                free(e->aliases[e->n_aliases].file);
                free(e->aliases[e->n_aliases].alias);
        }
        free(e->aliases);
        free(e);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void rarconfig_destroy()
{
        pthread_mutex_lock(&config_mutex);
        if (ht) {
                hashtable_destroy(ht);
                ht = NULL;
        }
        pthread_mutex_unlock(&config_mutex);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static struct config_entry *alloc_entry(const char *path)
{
        struct hash_table_entry *hte;
        hte = hashtable_entry_alloc(ht, path);
        if (hte)
                return hte->user_data;
        return NULL;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __entry_set_password(struct config_entry *e,
                        struct child_node *cnode)
{
        size_t len;
        wchar_t *wcs;
        char *s;
        char *tmp;

        s = strchr(cnode->value, '"');
        if (!s)
                return;
        tmp = strrchr(++s, '"');
        if (!tmp)
                return;
        *tmp = 0;

        e->password = strdup(s);

        len = mbstowcs(NULL, e->password, 0);
        if (len != (size_t)-1) {
                wcs = calloc(len + 1, sizeof(wchar_t));
                if (wcs != NULL) {
                        (void)mbstowcs(wcs, e->password, len + 1);
                        e->password_w = wcsdup(wcs);
                        free(wcs);
                }
        }
        e->mask |= RAR_PASSWORD_PROP;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __entry_set_seek_length(struct config_entry *e,
                        struct child_node *cnode)
{
        e->seek_length = strtoul(cnode->value, NULL, 0);
        e->mask |= RAR_SEEK_LENGTH_PROP;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __entry_set_save_eof(struct config_entry *e,
                        struct child_node *cnode)
{
        if (!strcasecmp(cnode->value, "true")) {
                e->save_eof = 1;
                e->mask |= RAR_SAVE_EOF_PROP;
        } else if (!strcasecmp(cnode->value, "false")) {
                e->save_eof = 0;
                e->mask |= RAR_SAVE_EOF_PROP;
        }
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __dirlevels(const char *path)
{
        char *tmp = strdup(path);
        char *tmp2 = tmp;
        int count = 0;

        while (strcmp(tmp, "/")) {
                ++count;
                tmp = __gnu_dirname(tmp);
        }
        free(tmp2);

        return count;
}
/*!
 *****************************************************************************
 *
 ****************************************************************************/
static int __check_paths(const char *a, const char *b)
{
        char *a_safe;
        char *b_safe;

        if (a[0] != '/' || b[0] != '/')
                return 1;
        if (strlen(a) < 2 || strlen(b) < 2)
                return 1;
        if (__dirlevels(a) != __dirlevels(b))
                return 1;

        /* Next check will prevent anything but changes to basename.
         * This code might need to be revisted when support for aliasing
         * of directories are added. */
        a_safe = strdup(a);
        b_safe = strdup(b);
        if (strcmp(__gnu_dirname(a_safe), __gnu_dirname(b_safe))) {
                free(a_safe);
                free(b_safe);
                return 1;
        }
        free(a_safe);
        free(b_safe);

        return 0;
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
static void __entry_set_alias(struct config_entry *e, struct child_node *cnode)
{
        char *file = malloc(strlen(cnode->value) + 1);
        char *alias = malloc(strlen(cnode->value) + 1);

        if (sscanf(cnode->value, " \"%[^\"]%*[^,]%*[^\"]\" %[^\"]",
                                file, alias) == 2) {
                if (__check_paths(file, alias))
                        goto out;
                __set_alias(e, file, alias);
        }
out:
        free(file);
        free(alias);
}

/*!
 *****************************************************************************
 *
 ****************************************************************************/
void rarconfig_init(const char *source, const char *cfg)
{
        struct parent_node *p_next = NULL;
        struct parent_node *p;
        struct hash_table_ops ops = {
                .alloc = __alloc,
                .free = __free,
        };
        FILE *fp;

        pthread_mutex_lock(&config_mutex);
        if (ht) {
                pthread_mutex_unlock(&config_mutex);
                return;
        }

        if (!cfg) {
                char *cfg_file = malloc(strlen(source) + 16);
                strcpy(cfg_file, source);
                strcat(cfg_file, "/.rarconfig");
                fp = fopen(cfg_file, "r");
                free(cfg_file);
        } else {
                fp = fopen(cfg, "r");
        }
        if (!fp) {
                pthread_mutex_unlock(&config_mutex);
                return;
        }

        ht = hashtable_init(RARCONFIG_SZ, &ops);

        while ((p = find_next_parent(fp, p_next))) {
                struct config_entry *e = alloc_entry(p->name);
                struct child_node *cnode_next = NULL;
                struct child_node *cnode;
                while ((cnode = find_next_child(fp, p, cnode_next))) {
                        if (!strcasecmp(cnode->name, "save-eof"))
                                __entry_set_save_eof(e, cnode);
                        if (!strcasecmp(cnode->name, "seek-length"))
                                __entry_set_seek_length(e, cnode);
                        if (!strcasecmp(cnode->name, "password"))
                                __entry_set_password(e, cnode);
                        if (!strcasecmp(cnode->name, "alias"))
                                __entry_set_alias(e, cnode);
                        free_child(cnode_next);
                        cnode_next = cnode;
                }
                free_parent(p_next);
                p_next = p;
        }

        pthread_mutex_unlock(&config_mutex);
        fclose(fp);
}
