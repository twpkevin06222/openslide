/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
 *  Copyright (c) 2016 Markus Pöpping
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-zip.h"
#include "openslide-decode-jpeg.h"

#include <glib.h>
#include <stdlib.h>

/* open archive from zip_source_t, fills _openslide_ziphandle */
bool _openslide_zip_open_archive_from_source(zip_source_t *zs,
                                             struct _openslide_ziphandle *zh,
                                             GError **err) {
  zip_error_t ze;
  zip_error_init(&ze);

  zh->handle = zip_open_from_source(zs, ZIP_RDONLY, &ze);

  if (zh->handle == NULL || ze.zip_err || ze.sys_err) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_open_archive_from_source: libzip error: %s",
                zip_error_strerror(&ze));
    zip_error_fini(&ze);
    return false;
  }

  zip_error_fini(&ze);
  g_mutex_init(&zh->lock);
  return true;
}

/* open archive by filename, fills _openslide_ziphandle */
bool _openslide_zip_open_archive(const char *filename,
                                 struct _openslide_ziphandle *zh,
                                 GError **err) {
  int zerr_i = 0;

  zh->handle = zip_open(filename, ZIP_RDONLY, &zerr_i);

  if (zh->handle == NULL || zerr_i != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_open_archive: libzip error code %i opening %s",
                zerr_i, filename);
    return false;
  }

  g_mutex_init(&zh->lock);
  return true;
}

/* close archive and clear mutex; sets handle to NULL */
bool _openslide_zip_close_archive(struct _openslide_ziphandle *zh) {
  int ret = 0;
  if (zh->handle) {
    ret = zip_close(zh->handle);
    g_mutex_clear(&zh->lock);
    zh->handle = NULL;
  }
  return ret == 0;
}

/* locate file by name, trying both '/' and '\\' as path separator */
zip_int64_t _openslide_zip_name_locate(struct _openslide_ziphandle *zh,
                                       const char *filename,
                                       zip_flags_t flags) {
  zip_int64_t idx;
  gchar *name = g_strdup(filename);
  gchar *s;

  /* first try with backslashes */
  for (s = name; *s; s++) {
    if (*s == '/') { *s = '\\'; }
  }
  idx = zip_name_locate(zh->handle, name, flags);

  if (idx < 0) {
    bool has_slash = false;
    for (s = name; *s; s++) {
      if (*s == '\\') { *s = '/'; has_slash = true; }
    }
    if (has_slash) {
      idx = zip_name_locate(zh->handle, name, flags);
    }
  }

  g_free(name);
  return idx;
}

/* read raw file data from zip archive into a g_malloc'd buffer */
bool _openslide_zip_read_file_data(struct _openslide_ziphandle *zh,
                                   zip_uint64_t index,
                                   gpointer *file_buf,
                                   gsize *file_len,
                                   GError **err) {
  zip_stat_t zstat;
  zip_int64_t bytes_read;
  zip_t *z = zh->handle;

  *file_buf = NULL;
  *file_len = 0;

  g_mutex_lock(&zh->lock);

  zip_stat_init(&zstat);
  zip_error_clear(z);

  if (zip_stat_index(z, index, 0, &zstat) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_read_file_data: zip_stat_index failed at"
                " index %"PRIu64": %s",
                (uint64_t)index, zip_error_strerror(zip_get_error(z)));
    goto FAIL_AND_UNLOCK;
  }

  const char *fname = (zstat.valid & ZIP_STAT_NAME) ? zstat.name : "<invalid>";

  if (!(zstat.valid & ZIP_STAT_SIZE)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_read_file_data: cannot get size of \"%s\""
                " (index %"PRIu64")",
                fname, (uint64_t)zstat.index);
    goto FAIL_AND_UNLOCK;
  }

  if (zstat.size >= ((uint64_t)1 << 31)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_read_file_data: file \"%s\" (index %"PRIu64")"
                " exceeds 2 GB limit",
                fname, (uint64_t)zstat.index);
    goto FAIL_AND_UNLOCK;
  }

  *file_buf = g_malloc(zstat.size);
  *file_len = zstat.size;

  zip_file_t *file = zip_fopen_index(z, index, 0);
  if (!file) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_read_file_data: cannot open \"%s\""
                " (index %"PRIu64"): %s",
                fname, (uint64_t)index, zip_error_strerror(zip_get_error(z)));
    goto FAIL_AND_UNLOCK;
  }

  bytes_read = zip_fread(file, *file_buf, zstat.size);
  int close_err = zip_fclose(file);

  if (close_err) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_read_file_data: zip_fclose error %i for"
                " \"%s\": %s",
                close_err, fname, zip_error_strerror(zip_get_error(z)));
    goto FAIL_AND_UNLOCK;
  }

  if ((gsize)bytes_read != zstat.size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_read_file_data: short read of \"%s\":"
                " got %"PRId64" of %"PRIu64" bytes",
                fname, (int64_t)bytes_read, (uint64_t)zstat.size);
    goto FAIL_AND_UNLOCK;
  }

  g_mutex_unlock(&zh->lock);
  return true;

FAIL_AND_UNLOCK:
  g_free(*file_buf);
  *file_buf = NULL;
  *file_len = 0;
  g_mutex_unlock(&zh->lock);
  return false;
}

/*
 * Decode an image from zip archive at the given index into a g_malloc'd
 * RGBA buffer.  The caller must g_free() the returned buffer.
 */
bool _openslide_zip_read_image(struct _openslide_ziphandle *zh,
                               zip_uint64_t file_id,
                               enum image_format dzif,
                               uint32_t **pdestbuf,
                               int32_t *pw,
                               int32_t *ph,
                               GError **err) {
  gpointer cbuf;
  gsize cbufsize;
  int32_t dw, dh;
  uint32_t *destbuf = NULL;
  bool success;

  *pdestbuf = NULL;
  *pw = -1;
  *ph = -1;

  success = _openslide_zip_read_file_data(zh, file_id, &cbuf, &cbufsize, err);
  if (!success) {
    return false;
  }

  if (dzif == IMAGE_FORMAT_JPEG) {
    success = _openslide_jpeg_decode_buffer_dimensions(cbuf, cbufsize,
                                                       &dw, &dh, err);
    if (success) {
      /* allocate with g_malloc so the cache can free with g_free */
      destbuf = g_new(uint32_t, (gsize)dw * dh);
      if (destbuf) {
        success = _openslide_jpeg_decode_buffer(cbuf, cbufsize,
                                                destbuf, dw, dh, err);
        if (success) {
          *pdestbuf = destbuf;
          *pw = dw;
          *ph = dh;
        } else {
          g_free(destbuf);
        }
      } else {
        success = false;
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "_openslide_zip_read_image: out of memory");
      }
    }
  } else if (dzif == IMAGE_FORMAT_PNG) {
    success = false;
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_read_image: PNG format not supported");
  } else {
    success = false;
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_zip_read_image: unsupported image format %d",
                (int)dzif);
  }

  g_free(cbuf);
  return success;
}
