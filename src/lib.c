//
//  Copyright (C) 2011-2013  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "lib.h"
#include "tree.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_SEARCH_PATHS 64

struct lib_unit {
   tree_t        top;
   tree_kind_t   kind;
   tree_rd_ctx_t read_ctx;
   bool          dirty;
   lib_mtime_t   mtime;
};

struct lib_index {
   ident_t           name;
   tree_kind_t       kind;
   struct lib_index *next;
};

struct lib {
   char              path[PATH_MAX];
   ident_t           name;
   unsigned          n_units;
   unsigned          units_alloc;
   struct lib_unit  *units;
   struct lib_index *index;
};

struct lib_list {
   lib_t           item;
   struct lib_list *next;
};

static lib_t           work = NULL;
static struct lib_list *loaded = NULL;

static ident_t upcase_name(const char *name)
{
   const char *last_slash = strrchr(name, '/');

   char *name_up = strdup((last_slash != NULL) ? last_slash + 1 : name);
   for (char *p = name_up; *p != '\0'; p++)
      *p = toupper((uint8_t)*p);

   ident_t i = ident_new(name_up);
   free(name_up);
   return i;
}

static lib_t lib_init(const char *name, const char *rpath)
{
   struct lib *l = xmalloc(sizeof(struct lib));
   l->n_units = 0;
   l->units   = NULL;
   l->name    = upcase_name(name);
   l->index   = NULL;

   if (realpath(rpath, l->path) == NULL)
      strncpy(l->path, rpath, PATH_MAX);

   struct lib_list *el = xmalloc(sizeof(struct lib_list));
   el->item = l;
   el->next = loaded;
   loaded = el;

   fbuf_t *f = lib_fbuf_open(l, "_index", FBUF_IN);
   if (f != NULL) {
      ident_rd_ctx_t ictx = ident_read_begin(f);

      const int entries = read_u32(f);
      for (int i = 0; i < entries; i++) {
         ident_t name = ident_read(ictx);
         tree_kind_t kind = read_u16(f);
         assert(kind < T_LAST_TREE_KIND);

         struct lib_index *in = xmalloc(sizeof(struct lib_index));
         in->name = name;
         in->kind = kind;
         in->next = l->index;

         l->index = in;
      }

      ident_read_end(ictx);
      fbuf_close(f);
   }

   return l;
}

static struct lib_unit *lib_put_aux(lib_t lib, tree_t unit,
                                    tree_rd_ctx_t ctx, bool dirty,
                                    lib_mtime_t mtime)
{
   assert(lib != NULL);
   assert(unit != NULL);

   if (lib->n_units == 0) {
      lib->units_alloc = 16;
      lib->units = xmalloc(sizeof(struct lib_unit) * lib->units_alloc);
   }
   else if (lib->n_units == lib->units_alloc) {
      lib->units_alloc *= 2;
      lib->units = xrealloc(lib->units,
                            sizeof(struct lib_unit) * lib->units_alloc);
   }

   unsigned n = lib->n_units++;
   lib->units[n].top      = unit;
   lib->units[n].read_ctx = ctx;
   lib->units[n].dirty    = dirty;
   lib->units[n].mtime    = mtime;
   lib->units[n].kind     = tree_kind(unit);

   ident_t name = tree_ident(unit);
   struct lib_index *it;
   for (it = lib->index;
        (it != NULL) && (it->name != name);
        it = it->next)
      ;

   if (it == NULL) {
      struct lib_index *new = xmalloc(sizeof(struct lib_index));
      new->name = name;
      new->kind = tree_kind(unit);
      new->next = lib->index;

      lib->index = new;
   }
   else
      it->kind = tree_kind(unit);

   return &(lib->units[n]);
}

static lib_t lib_find_at(const char *name, const char *path)
{
   char dir[PATH_MAX];
   snprintf(dir, sizeof(dir), "%s/%s", path, name);

   // Convert to lower case
   for (char *p = dir; *p != '\0'; p++)
      *p = tolower((uint8_t)*p);

   if (access(dir, F_OK) < 0)
      return NULL;

   char marker[PATH_MAX];
   snprintf(marker, sizeof(marker), "%s/_NVC_LIB", dir);
   if (access(marker, F_OK) < 0)
      return NULL;

   return lib_init(name, dir);
}

static const char *lib_file_path(lib_t lib, const char *name)
{
   static char buf[PATH_MAX];
   snprintf(buf, sizeof(buf), "%s/%s", lib->path, name);
   return buf;
}

lib_t lib_new(const char *name)
{
   if (access(name, F_OK) == 0) {
      errorf("file %s already exists", name);
      return NULL;
   }

   if (mkdir(name, 0777) != 0) {
      perror("mkdir");
      return NULL;
   }

   lib_t l = lib_init(name, name);

   FILE *tag = lib_fopen(l, "_NVC_LIB", "w");
   fprintf(tag, "%s\n", PACKAGE_STRING);
   fclose(tag);

   return l;
}

lib_t lib_tmp(void)
{
   // For unit tests, avoids creating files
   return lib_init("work", "");
}

static void push_path(const char **base, size_t *pidx, const char *path)
{
   if (*pidx < MAX_SEARCH_PATHS - 1) {
      base[(*pidx)++] = path;
      base[*pidx] = NULL;
   }
}

lib_t lib_find(const char *name, bool verbose, bool search)
{
   // Search in already loaded libraries
   ident_t name_i = upcase_name(name);
   for (struct lib_list *it = loaded; it != NULL; it = it->next) {
      if (lib_name(it->item) == name_i)
         return it->item;
   }

   const char *paths[MAX_SEARCH_PATHS];
   size_t idx = 0;

   char *name_copy = strdup(name);
   char *sep = strrchr(name_copy, '/');
   if (sep == NULL)
      push_path(paths, &idx, ".");
   else {
      // Work library contains path
      *sep = '\0';
      push_path(paths, &idx, name_copy);
      name = sep + 1;
   }

   char *env_copy = NULL;
   if (search) {
      const char *libpath_env = getenv("NVC_LIBPATH");
      if (libpath_env) {
         env_copy = strdup(libpath_env);

         const char *path_tok = strtok(env_copy, ":");
         do {
            push_path(paths, &idx, path_tok);
         } while ((path_tok = strtok(NULL, ":")));
      }

      push_path(paths, &idx, DATADIR);
   }

   lib_t lib;
   for (const char **p = paths; *p != NULL; p++) {
      if ((lib = lib_find_at(name, *p))) {
         free(name_copy);
         free(env_copy);
         return lib;
      }
   }

   if (verbose) {
      errorf("library %s not found in:", name);
      for (const char **p = paths; *p != NULL; p++) {
         fprintf(stderr, "  %s\n", *p);
      }
   }

   free(name_copy);
   free(env_copy);
   return NULL;
}

FILE *lib_fopen(lib_t lib, const char *name, const char *mode)
{
   assert(lib != NULL);
   return fopen(lib_file_path(lib, name), mode);
}

fbuf_t *lib_fbuf_open(lib_t lib, const char *name, fbuf_mode_t mode)
{
   assert(lib != NULL);
   return fbuf_open(lib_file_path(lib, name), mode);
}

void lib_free(lib_t lib)
{
   assert(lib != NULL);

   for (struct lib_list *it = loaded, *prev = NULL;
        it != NULL; loaded = it, it = it->next) {

      if (it->item == lib) {
         if (prev)
            prev->next = it->next;
         else
            loaded = it->next;
         free(it);
         break;
      }
   }

   if (lib->units != NULL)
      free(lib->units);
   free(lib);
}

void lib_destroy(lib_t lib)
{
   // This is convenience function for testing: remove all
   // files associated with a library

   assert(lib != NULL);

   DIR *d = opendir(lib->path);
   if (d == NULL) {
      perror("opendir");
      return;
   }

   char buf[PATH_MAX];
   struct dirent *e;
   while ((e = readdir(d))) {
      if (e->d_name[0] != '.') {
         snprintf(buf, sizeof(buf), "%s/%s", lib->path, e->d_name);
         if (unlink(buf) < 0)
            perror("unlink");
      }
   }

   closedir(d);

   if (rmdir(lib->path) < 0)
      perror("rmdir");
}

lib_t lib_work(void)
{
   assert(work != NULL);
   return work;
}

void lib_set_work(lib_t lib)
{
   work = lib;
}

static lib_mtime_t lib_time_to_usecs(time_t t)
{
   return (lib_mtime_t)t * 1000 * 1000;
}

void lib_put(lib_t lib, tree_t unit)
{
   lib_mtime_t usecs = lib_time_to_usecs(time(NULL));
   lib_put_aux(lib, unit, NULL, true, usecs);
}

static struct lib_unit *lib_get_aux(lib_t lib, ident_t ident)
{
   assert(lib != NULL);

   // Search in the list of already loaded libraries
   for (unsigned n = 0; n < lib->n_units; n++) {
      if (tree_ident(lib->units[n].top) == ident)
         return &(lib->units[n]);
   }

   if (*(lib->path) == '\0')   // Temporary library
      return NULL;

   // Otherwise search in the filesystem
   DIR *d = opendir(lib->path);
   if (d == NULL)
      fatal("%s: %s", lib->path, strerror(errno));

   struct lib_unit *unit = NULL;
   const char *search = istr(ident);
   struct dirent *e;
   while ((e = readdir(d))) {
      if (strcmp(e->d_name, search) == 0) {
         fbuf_t *f = lib_fbuf_open(lib, e->d_name, FBUF_IN);
         tree_rd_ctx_t ctx = tree_read_begin(f, lib_file_path(lib, e->d_name));
         tree_t top = tree_read(ctx);

         struct stat st;
         if (stat(lib_file_path(lib, e->d_name), &st) < 0)
            fatal_errno("%s", e->d_name);

         lib_mtime_t mt = lib_time_to_usecs(st.st_mtime);
#if defined HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC
         mt += st.st_mtimespec.tv_nsec / 1000;
#elif defined HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC
         mt += st.st_mtim.tv_nsec / 1000;
#endif

         unit = lib_put_aux(lib, top, ctx, false, mt);
         break;
      }
   }

   closedir(d);
   return unit;
}

lib_mtime_t lib_mtime(lib_t lib, ident_t ident)
{
   struct lib_unit *lu = lib_get_aux(lib, ident);
   assert(lu != NULL);
   return lu->mtime;
}

tree_t lib_get_ctx(lib_t lib, ident_t ident, tree_rd_ctx_t *ctx)
{
   struct lib_unit *lu = lib_get_aux(lib, ident);
   if (lu != NULL) {
      *ctx = lu->read_ctx;
      return lu->top;
   }
   else
      return NULL;
}

tree_t lib_get(lib_t lib, ident_t ident)
{
   struct lib_unit *lu = lib_get_aux(lib, ident);
   if (lu != NULL) {
      if (lu->read_ctx != NULL) {
         tree_read_end(lu->read_ctx);
         lu->read_ctx = NULL;
      }
      return lu->top;
   }
   else
      return NULL;
}

ident_t lib_name(lib_t lib)
{
   assert(lib != NULL);
   return lib->name;
}

void lib_save(lib_t lib)
{
   assert(lib != NULL);

   for (unsigned n = 0; n < lib->n_units; n++) {
      if (lib->units[n].dirty) {
         const char *name = istr(tree_ident(lib->units[n].top));
         fbuf_t *f = lib_fbuf_open(lib, name, FBUF_OUT);
         if (f == NULL)
            fatal("failed to create %s in library %s", name, istr(lib->name));
         tree_wr_ctx_t ctx = tree_write_begin(f);
         tree_write(lib->units[n].top, ctx);
         tree_write_end(ctx);
         fbuf_close(f);

         lib->units[n].dirty = false;
      }
   }

   struct lib_index *it;
   int index_sz = 0;
   for (it = lib->index; it != NULL; it = it->next, ++index_sz)
      ;

   fbuf_t *f = lib_fbuf_open(lib, "_index", FBUF_OUT);
   if (f == NULL)
      fatal("failed to create library %s index", istr(lib->name));

   ident_wr_ctx_t ictx = ident_write_begin(f);

   write_u32(index_sz, f);
   for (it = lib->index; it != NULL; it = it->next) {
      ident_write(it->name, ictx);
      write_u16(it->kind, f);
   }

   ident_write_end(ictx);
   fbuf_close(f);
}

void lib_walk_index(lib_t lib, lib_index_fn_t fn, void *context)
{
   assert(lib != NULL);

   struct lib_index *it;
   for (it = lib->index; it != NULL; it = it->next)
      (*fn)(it->name, it->kind, context);
}

void lib_realpath(lib_t lib, const char *name, char *buf, size_t buflen)
{
   assert(lib != NULL);

   if (name)
      snprintf(buf, buflen, "%s/%s", lib->path, name);
   else
      strncpy(buf, lib->path, buflen);
}

void lib_mkdir(lib_t lib, const char *name)
{
   if ((mkdir(lib_file_path(lib, name), 0777) != 0)
       && (errno != EEXIST))
      fatal_errno("mkdir");
}
