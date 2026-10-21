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

#ifndef OPENSLIDE_OPENSLIDE_DECODE_ZIP_H_
#define OPENSLIDE_OPENSLIDE_DECODE_ZIP_H_

#include "openslide-private.h"

#include <glib.h>
#include <zip.h>

enum image_format {
  IMAGE_FORMAT_UNKNOWN = 0,
  IMAGE_FORMAT_JPEG,
  IMAGE_FORMAT_PNG,
  IMAGE_FORMAT_BMP,
  IMAGE_FORMAT_JP2K,
};

/* zip handle with embedded mutex for thread-safe access */
struct _openslide_ziphandle {
  zip_t *handle;
  GMutex lock;
};

/* opens zip archive by filename */
bool _openslide_zip_open_archive(const char *filename,
                                 struct _openslide_ziphandle *zh,
                                 GError **err);

/* opens zip archive from an already-created zip_source_t */
bool _openslide_zip_open_archive_from_source(zip_source_t *zs,
                                             struct _openslide_ziphandle *zh,
                                             GError **err);

/* closes zip archive and resets handle to NULL */
bool _openslide_zip_close_archive(struct _openslide_ziphandle *zh);

/* locates a file by name, trying both '/' and '\\' path separators */
zip_int64_t _openslide_zip_name_locate(struct _openslide_ziphandle *zh,
                                       const char *filename,
                                       zip_flags_t flags);

/* reads raw file data from zip archive into a g_malloc'd buffer */
bool _openslide_zip_read_file_data(struct _openslide_ziphandle *zh,
                                   zip_uint64_t index,
                                   gpointer *file_buf,
                                   gsize *file_len,
                                   GError **err);

/* reads and decodes an image from zip archive into a g_malloc'd RGBA buffer */
bool _openslide_zip_read_image(struct _openslide_ziphandle *zh,
                               zip_uint64_t file_id,
                               enum image_format dzif,
                               uint32_t **pdestbuf,
                               int32_t *pw,
                               int32_t *ph,
                               GError **err);

#endif /* OPENSLIDE_OPENSLIDE_DECODE_ZIP_H_ */
