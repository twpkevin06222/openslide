/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
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

/*
 * PreciPoint M8-VMIC support.
 *
 * A .vmic file is a ZIP archive (outer) containing a second ZIP archive
 * (inner, named "Image.vmici" or "Image") that holds a DeepZoom JPEG tile
 * pyramid plus configuration files.
 *
 * Quickhash: DZI XML + config.osc XML + raw bytes of the single-tile image
 * at the highest fully-tiled level.
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-xml.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-zip.h"
#include "openslide-hash.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>

/* ---------- constants ---------------------------------------------------- */

static const char _PRECIPOINT_VENDOR[] = "PreciPoint";
static const char _PRECIPOINT_VMICTYPE[] = "M8-VMIC";

/* either of these names is accepted for the inner archive */
static const char _PRECIPOINT_INNER_CONTAINER_SUFFIX[] = ".vmici";
static const char _PRECIPOINT_INNER_CONTAINER_LEGACY_NAME[] = "Image";

static const char _PRECIPOINT_MACRO_IMAGE[] = "VMCF/overview.jpg";
static const char _PRECIPOINT_PROPS_FILENAME[] = "VMCF/config.osc";
static const char _PRECIPOINT_PROPS_OSC_NODE[] = "ObjectScanConfig";
static const char _PRECIPOINT_PROPS_PREFIX[] = "PreciPoint";
static const char _PRECIPOINT_PROPPATH_MAGNIFICATION[] =
  "PreciPoint.Objective.Magnification";
static const char _PRECIPOINT_PROPPATH_NAME[] = "PreciPoint.ScanData.Name";

static const char _DEEPZOOM_PROP_IMAGE_NODE[] = "Image";
static const char _DEEPZOOM_PROP_PPM[] = "PixelPerMicron";
static const char _DEEPZOOM_PROP_TILESIZE[] = "TileSize";
static const char _DEEPZOOM_PROP_SIZE_NODE[] = "Size";
static const char _DEEPZOOM_PROP_WIDTH[] = "Width";
static const char _DEEPZOOM_PROP_HEIGHT[] = "Height";
static const char _DEEPZOOM_PROP_OVERLAP[] = "Overlap";
static const char _DEEPZOOM_PROP_IMAGE_FORMAT[] = "Format";

/* ---------- structs ------------------------------------------------------- */

#define MAX_IFSTR 10
#define MAX_DZFILENAME 50

/* DeepZoom pyramid parameters parsed from the .dzi/.xml key file */
struct dzinfo {
  int32_t dz_level_count;
  int32_t tilesize;
  int32_t overlap;
  int64_t width, height;
  gchar tile_imgformat_str[MAX_IFSTR + 1];
  enum image_format tile_format_id;
  int32_t os_level_count;
  int32_t dz_one_tile_level;
  gchar key_filename[MAX_DZFILENAME + 1];
  gchar folder_name[MAX_DZFILENAME + 1];
};

/* Top-level slide data; dzinfo must be first for pointer casting */
struct vmicinfo {
  struct dzinfo base;
  struct _openslide_ziphandle zip_outer;
  struct _openslide_ziphandle zi;
  int64_t zip_entries;
};

/* Per-tile pre-indexed metadata */
struct dz_tileinfo {
  int32_t w, h;
  zip_int64_t zipindex;
};

/* One OpenSlide level backed by a DeepZoom level */
struct dz_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
  int dz_level_id;
  int cols;
  int rows;
  struct dz_tileinfo *matrix;
};

/* Associated image backed by a zip entry */
struct zip_associated_image {
  struct _openslide_associated_image base;
  struct _openslide_ziphandle *refzip;
  zip_uint64_t zipindex;
};

/* ---------- helper: find XML node by name --------------------------------- */

static xmlNode *xml_find_node(xmlNode *node, const xmlChar *name) {
  for (xmlNode *cur = node; cur != NULL; cur = cur->next) {
    if (xmlStrcmp(cur->name, name) == 0) {
      return cur;
    }
  }
  return NULL;
}

/* ---------- DeepZoom XML parsing ----------------------------------------- */

static bool dzz_get_deepzoom_properties(xmlDoc *xmldoc, struct dzinfo *dzi,
                                        GHashTable *properties,
                                        GError **err) {
  xmlNode *img = xml_find_node(xmldoc->children,
                               (xmlChar *)_DEEPZOOM_PROP_IMAGE_NODE);
  if (!img) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DZC/XML: cannot find <%s> element",
                _DEEPZOOM_PROP_IMAGE_NODE);
    return false;
  }

  double ppm = _openslide_xml_parse_double_attr(img, _DEEPZOOM_PROP_PPM, err);
  if (*err) { return false; }
  if (ppm == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DZC/XML: PixelPerMicron is zero");
    return false;
  }

  g_hash_table_insert(properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                      _openslide_format_double(1.0 / ppm));
  g_hash_table_insert(properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                      _openslide_format_double(1.0 / ppm));

  dzi->tilesize = _openslide_xml_parse_int_attr(img,
                                                _DEEPZOOM_PROP_TILESIZE,
                                                err);
  dzi->overlap = _openslide_xml_parse_int_attr(img,
                                               _DEEPZOOM_PROP_OVERLAP,
                                               err);
  if (*err) { return false; }

  if (dzi->tilesize <= 0 || dzi->overlap < 0 ||
      dzi->overlap >= dzi->tilesize / 2) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DZC/XML: invalid tilesize=%d or overlap=%d",
                dzi->tilesize, dzi->overlap);
    return false;
  }

  xmlNode *sz = xml_find_node(img->children,
                              (xmlChar *)_DEEPZOOM_PROP_SIZE_NODE);
  if (!sz) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DZC/XML: cannot find <%s> element",
                _DEEPZOOM_PROP_SIZE_NODE);
    return false;
  }

  dzi->width = _openslide_xml_parse_int_attr(sz, _DEEPZOOM_PROP_WIDTH, err);
  dzi->height = _openslide_xml_parse_int_attr(sz, _DEEPZOOM_PROP_HEIGHT, err);
  if (*err) { return false; }
  if (dzi->width <= 0 || dzi->height <= 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DZC/XML: invalid dimensions %"PRId64"x%"PRId64,
                dzi->width, dzi->height);
    return false;
  }

  xmlChar *fmt = xmlGetProp(img, (const xmlChar *)_DEEPZOOM_PROP_IMAGE_FORMAT);
  if (!fmt) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DZC/XML: missing Format attribute");
    return false;
  }
  g_strlcpy(dzi->tile_imgformat_str, (const gchar *)fmt, MAX_IFSTR);
  dzi->tile_format_id = IMAGE_FORMAT_UNKNOWN;
  if (g_ascii_strcasecmp((const gchar *)fmt, "jpg") == 0 ||
      g_ascii_strcasecmp((const gchar *)fmt, "jpeg") == 0) {
    dzi->tile_format_id = IMAGE_FORMAT_JPEG;
  } else if (g_ascii_strcasecmp((const gchar *)fmt, "png") == 0) {
    dzi->tile_format_id = IMAGE_FORMAT_PNG;
  } else if (g_ascii_strcasecmp((const gchar *)fmt, "bmp") == 0) {
    dzi->tile_format_id = IMAGE_FORMAT_BMP;
  }
  xmlFree(fmt);

  if (dzi->tile_format_id == IMAGE_FORMAT_UNKNOWN) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DZC/XML: unrecognized tile format \"%s\"",
                dzi->tile_imgformat_str);
    return false;
  }
  if (dzi->tile_format_id != IMAGE_FORMAT_JPEG) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DZC/XML: only JPEG tiles are currently supported");
    return false;
  }

  /* compute DeepZoom level count */
  int longside = MAX(dzi->width, dzi->height);
  int cnt = 1;
  dzi->os_level_count = 0;
  g_assert(longside > 0);
  while (longside > 1) {
    if (dzi->os_level_count == 0 && longside <= dzi->tilesize) {
      dzi->os_level_count = cnt;
    }
    longside = (longside + 1) >> 1;
    cnt++;
  }
  dzi->dz_level_count = cnt;
  dzi->dz_one_tile_level = cnt - dzi->os_level_count;

  return true;
}

/* Search the archive for the .dzi or .xml key file (not inside a folder). */
static bool dzz_find_key_file(zip_t *z, struct dzinfo *dzi) {
  zip_int64_t count = zip_get_num_entries(z, 0);
  zip_int64_t found = -1;
  const char *name = NULL;

  for (zip_int64_t i = 0; i < count; i++) {
    name = zip_get_name(z, i, ZIP_FL_ENC_RAW);
    if (!name) { continue; }
    if (strchr(name, '\\') || strchr(name, '/')) { continue; }
    if (g_str_has_suffix(name, ".dzi") || g_str_has_suffix(name, ".xml") ||
        g_str_has_suffix(name, ".DZI") || g_str_has_suffix(name, ".XML")) {
      found = i;
      break;
    }
  }

  if (found < 0) { return false; }

  g_strlcpy(dzi->key_filename, name, MAX_DZFILENAME - 2);
  g_strlcpy(dzi->folder_name, name, MAX_DZFILENAME - 2);
  /* replace last 4 chars (".xml" / ".dzi") with "_files" */
  size_t base_len = strlen(dzi->folder_name) - 4;
  memcpy(dzi->folder_name + base_len, "_files", 7);
  return true;
}

/* ---------- XML file loader from ZIP ------------------------------------- */

static xmlDoc *zip_parse_xml_file(struct _openslide_ziphandle *zh,
                                  const char *filename,
                                  zip_flags_t flags,
                                  struct _openslide_hash *hash,
                                  GError **err) {
  zip_int64_t file_id = _openslide_zip_name_locate(zh, filename, flags);
  if (file_id == -1) {
    zip_error_t *ze = zip_get_error(zh->handle);
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "VMIC: cannot find \"%s\" in archive: %s",
                filename, zip_error_strerror(ze));
    return NULL;
  }

  gpointer xmlbuf = NULL;
  gsize xmlsize = 0;
  if (!_openslide_zip_read_file_data(zh, file_id, &xmlbuf, &xmlsize, err)) {
    g_prefix_error(err, "VMIC: cannot read \"%s\": ", filename);
    return NULL;
  }

  if (hash) {
    _openslide_hash_data(hash, xmlbuf, xmlsize);
  }

  xmlDoc *doc = xmlReadMemory(xmlbuf, xmlsize, NULL, NULL,
                              XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
                              XML_PARSE_NONET);
  g_free(xmlbuf);

  if (!doc) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "VMIC: cannot parse XML in \"%s\"", filename);
    return NULL;
  }
  return doc;
}

/* ---------- property tree dump ------------------------------------------- */

static void xml_tree_to_properties(xmlNode *node, GHashTable *props,
                                   const char *prefix) {
  for (; node; node = node->next) {
    if (node->type == XML_ELEMENT_NODE) {
      gchar *ename = g_strconcat(prefix, ".", node->name, NULL);
      for (xmlAttr *attr = node->properties; attr; attr = attr->next) {
        xmlChar *val = xmlGetProp(node, attr->name);
        gchar *key = g_strconcat(ename, ".", attr->name, NULL);
        g_hash_table_insert(props, key, g_strdup((gchar *)val));
        xmlFree(val);
      }
      if (node->children) {
        xml_tree_to_properties(node->children, props, ename);
      }
      g_free(ename);
    } else if (node->type == XML_TEXT_NODE) {
      xmlChar *content = xmlNodeGetContent(node);
      g_hash_table_insert(props,
                          g_strdup(prefix),
                          g_strdup((gchar *)content));
      xmlFree(content);
    }
  }
}

/* ---------- container open / close --------------------------------------- */

static struct vmicinfo *vmic_open_container(const char *filename,
                                            bool detection_only,
                                            GError **err) {
  struct vmicinfo *vmic = g_new0(struct vmicinfo, 1);

  if (!_openslide_zip_open_archive(filename, &vmic->zip_outer, err)) {
    goto FAIL;
  }

  zip_t *zo = vmic->zip_outer.handle;
  zip_int64_t outer_count = zip_get_num_entries(zo, 0);
  int vmici_index = -1;

  /* scan only first 20 entries to avoid slowdown on large archives */
  for (int i = 0; i < MIN(outer_count, 20); i++) {
    const char *name = zip_get_name(zo, i, ZIP_FL_ENC_RAW);
    if (!name) { continue; }
    if (!g_str_has_suffix(name, _PRECIPOINT_INNER_CONTAINER_SUFFIX) &&
        strcmp(name, _PRECIPOINT_INNER_CONTAINER_LEGACY_NAME) != 0) {
      continue;
    }
    /* verify ZIP magic PK\x03\x04 */
    zip_file_t *file = zip_fopen_index(zo, i, 0);
    if (file) {
      uint32_t magic = 0;
      zip_fread(file, &magic, 4);
      zip_fclose(file);
      if (magic == 0x04034B50) {
        vmici_index = i;
        break;
      }
    }
  }

  if (vmici_index < 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "VMIC: inner image container not found");
    goto FAIL_AND_CLOSE_OUTER;
  }

  if (detection_only) {
    _openslide_zip_close_archive(&vmic->zip_outer);
    return vmic;
  }

  zip_source_t *zs = zip_source_zip(zo, zo, vmici_index,
                                    ZIP_FL_UNCHANGED, 0, 0);
  if (!zs) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "VMIC: cannot create zip source for inner container at"
                " index %d", vmici_index);
    goto FAIL_AND_CLOSE_OUTER;
  }

  if (!_openslide_zip_open_archive_from_source(zs, &vmic->zi, err)) {
    zip_source_free(zs);
    goto FAIL_AND_CLOSE_OUTER;
  }

  vmic->zip_entries = zip_get_num_entries(vmic->zi.handle, 0);
  return vmic;

FAIL_AND_CLOSE_OUTER:
  _openslide_zip_close_archive(&vmic->zip_outer);
FAIL:
  g_free(vmic);
  return NULL;
}

static void vmic_close_container(struct vmicinfo *vmic) {
  if (!vmic) { return; }
  if (vmic->zi.handle) { _openslide_zip_close_archive(&vmic->zi); }
  if (vmic->zip_outer.handle) { _openslide_zip_close_archive(&vmic->zip_outer); }
}

/* ---------- property gathering ------------------------------------------- */

static bool vmic_get_properties(openslide_t *osr,
                                struct _openslide_hash *quickhash,
                                GError **err) {
  struct vmicinfo *vmic = (struct vmicinfo *)osr->data;
  struct dzinfo *dzi = (struct dzinfo *)osr->data;

  if (!dzz_find_key_file(vmic->zi.handle, dzi)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "VMIC: cannot find DeepZoom key file (.dzi/.xml) in archive");
    return false;
  }

  xmlDoc *doc = zip_parse_xml_file(&vmic->zi, dzi->key_filename,
                                   ZIP_FL_ENC_RAW | ZIP_FL_NOCASE,
                                   quickhash, err);
  if (!doc) { return false; }
  bool ok = dzz_get_deepzoom_properties(doc, dzi, osr->properties, err);
  xmlFreeDoc(doc);
  if (!ok) { return false; }

  if (dzi->overlap != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "VMIC: non-zero tile overlap (%d) is not supported",
                dzi->overlap);
    return false;
  }

  doc = zip_parse_xml_file(&vmic->zi, _PRECIPOINT_PROPS_FILENAME,
                            ZIP_FL_ENC_RAW | ZIP_FL_NOCASE,
                            quickhash, err);
  if (!doc) { return false; }

  xmlNode *osc = xml_find_node(doc->children,
                               (xmlChar *)_PRECIPOINT_PROPS_OSC_NODE);
  if (!osc) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "VMIC: cannot find <%s> node in %s",
                _PRECIPOINT_PROPS_OSC_NODE, _PRECIPOINT_PROPS_FILENAME);
    xmlFreeDoc(doc);
    return false;
  }

  xml_tree_to_properties(osc->children, osr->properties,
                         _PRECIPOINT_PROPS_PREFIX);
  xmlFreeDoc(doc);

  _openslide_duplicate_double_prop(osr, _PRECIPOINT_PROPPATH_MAGNIFICATION,
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);

  const char *name_val = g_hash_table_lookup(osr->properties,
                                             _PRECIPOINT_PROPPATH_NAME);
  if (name_val) {
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_COMMENT),
                        g_strdup(name_val));
  }
  return true;
}

/* ---------- tile pre-indexing -------------------------------------------- */

static void dzz_find_indices(struct dzinfo *dzi, zip_t *z,
                             const char *folder, int level,
                             int rows, int cols,
                             struct dz_tileinfo *matrix) {
  gchar *prefix1 = g_strdup_printf("%s\\%d\\", folder, level);
  gchar *prefix2 = g_strdup_printf("%s/%d/", folder, level);
  unsigned int plen = strlen(prefix1);
  g_assert(plen == strlen(prefix2));

  for (int i = 0; i < rows * cols; i++) {
    matrix[i].zipindex = -1;
  }

  zip_int64_t total = zip_get_num_entries(z, 0);
  for (zip_int64_t i = 0; i < total; i++) {
    const char *fname = zip_get_name(z, i, ZIP_FL_ENC_RAW);
    if (!fname) { continue; }

    if (memcmp(fname, prefix1, plen) != 0 &&
        memcmp(fname, prefix2, plen) != 0) {
      continue;
    }

    char *s = (char *)fname + plen;
    int x = (int)strtol(s, &s, 10);
    if (!*s || *s++ != '_') { continue; }
    int y = (int)strtol(s, &s, 10);
    if (!*s || *s++ != '.') { continue; }
    if (strcmp(s, dzi->tile_imgformat_str) != 0) { continue; }

    if (x >= 0 && y >= 0 && x < cols && y < rows) {
      matrix[y * cols + x].zipindex = i;
    }
  }

  g_free(prefix2);
  g_free(prefix1);
}

/* ---------- tile reading -------------------------------------------------- */

/* forward-declared here so vmic_create_levels can pass it as a callback */
static bool vmic_read_tile(openslide_t *osr, cairo_t *cr,
                           struct _openslide_level *level,
                           int64_t tile_col, int64_t tile_row,
                           void *tile, void *arg, GError **err);

/* ---------- level creation / destruction ---------------------------------- */

static void vmic_destroy_levels(openslide_t *osr) {
  for (int32_t i = 0; i < osr->level_count; i++) {
    struct dz_level *l = (struct dz_level *)osr->levels[i];
    if (l) {
      g_free(l->matrix);
      _openslide_grid_destroy(l->grid);
      g_free(l);
    }
  }
  g_free(osr->levels);
}

static bool vmic_create_levels(openslide_t *osr, GError **err) {
  struct vmicinfo *vmic = (struct vmicinfo *)osr->data;
  struct dzinfo *dzi = (struct dzinfo *)osr->data;

  g_assert(osr->levels == NULL);
  osr->level_count = dzi->os_level_count;
  osr->levels = g_new0(struct _openslide_level *, osr->level_count);
  if (!osr->levels) { goto FAIL; }

  int w = dzi->width;
  int h = dzi->height;
  int tilesize = dzi->tilesize;
  int overlap = dzi->overlap;
  double ds = 1.0;
  int os_id = 0;

  for (int dz_id = dzi->dz_level_count - 1;
       dz_id >= dzi->dz_one_tile_level;
       dz_id--) {

    struct dz_level *l = g_new0(struct dz_level, 1);
    if (!l) { goto FAIL_AND_DESTROY; }

    int across = (w + tilesize - 1) / tilesize;
    int down = (h + tilesize - 1) / tilesize;

    l->base.tile_w = tilesize;
    l->base.tile_h = tilesize;
    l->base.w = w;
    l->base.h = h;
    l->base.downsample = ds;
    l->cols = across;
    l->rows = down;
    l->dz_level_id = dz_id;
    l->grid = _openslide_grid_create_tilemap(osr, tilesize, tilesize,
                                             vmic_read_tile, NULL);

    l->matrix = g_new(struct dz_tileinfo, across * down);
    if (!l->matrix) {
      g_free(l);
      goto FAIL_AND_DESTROY;
    }

    dzz_find_indices(dzi, vmic->zi.handle, vmic->base.folder_name,
                     dz_id, down, across, l->matrix);

    int rem_y = h;
    for (int y = 0; y < down; y++) {
      int tile_h = MIN(rem_y, tilesize);
      double off_y = 0;
      if (y > 0) { tile_h += overlap; off_y = -overlap; }
      if (y < down - 1) { tile_h += overlap; }

      int rem_x = w;
      for (int x = 0; x < across; x++) {
        int tile_w = MIN(rem_x, tilesize);
        double off_x = 0;
        if (x > 0) { tile_w += overlap; off_x = -overlap; }
        if (x < across - 1) { tile_w += overlap; }

        struct dz_tileinfo *ti = &l->matrix[y * across + x];
        ti->w = tile_w;
        ti->h = tile_h;

        _openslide_grid_tilemap_add_tile(l->grid, x, y,
                                         off_x, off_y,
                                         tile_w, tile_h,
                                         NULL);
        rem_x -= tilesize;
      }
      rem_y -= tilesize;
    }

    osr->levels[os_id++] = (struct _openslide_level *)l;
    w = (w + 1) >> 1;
    h = (h + 1) >> 1;
    ds *= 2.0;
  }

  return true;

FAIL_AND_DESTROY:
  vmic_destroy_levels(osr);
FAIL:
  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "vmic_create_levels: allocation failure");
  return false;
}

/* ---------- tile reading (definition) ------------------------------------ */

static bool vmic_read_tile(openslide_t *osr,
                           cairo_t *cr,
                           struct _openslide_level *level,
                           int64_t tile_col, int64_t tile_row,
                           void *tile G_GNUC_UNUSED,
                           void *arg G_GNUC_UNUSED,
                           GError **err) {
  struct dz_level *l = (struct dz_level *)level;
  struct vmicinfo *vmic = (struct vmicinfo *)osr->data;
  struct dz_tileinfo *ti = &l->matrix[tile_row * l->cols + tile_col];
  int32_t tw = ti->w;
  int32_t th = ti->h;
  zip_int64_t zipindex = ti->zipindex;

  struct _openslide_cache_entry *cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache, level,
                                             tile_col, tile_row,
                                             &cache_entry);

  if (!tiledata && zipindex != -1) {
    int32_t w, h;
    if (!_openslide_zip_read_image(&vmic->zi, zipindex,
                                    vmic->base.tile_format_id,
                                    &tiledata, &w, &h, err)) {
      g_prefix_error(err, "vmic_read_tile: ");
      return false;
    }

    if (w != tw || h != th) {
      g_free(tiledata);
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "vmic_read_tile: tile size mismatch at level=%d"
                  " col=%"PRId64" row=%"PRId64
                  ": expected %dx%d, got %dx%d",
                  l->dz_level_id, tile_col, tile_row,
                  tw, th, w, h);
      return false;
    }

    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
                         tiledata, (uint64_t)tw * th * 4, &cache_entry);
  }

  if (tiledata) {
    cairo_surface_t *surface =
      cairo_image_surface_create_for_data((unsigned char *)tiledata,
                                          CAIRO_FORMAT_ARGB32,
                                          tw, th, tw * 4);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_surface_destroy(surface);
    cairo_paint(cr);
    _openslide_cache_entry_unref(cache_entry);
  }

  return true;
}

static bool vmic_paint_region(G_GNUC_UNUSED openslide_t *osr,
                              cairo_t *cr,
                              int64_t x, int64_t y,
                              struct _openslide_level *level,
                              int32_t w, int32_t h,
                              GError **err) {
  struct dz_level *l = (struct dz_level *)level;
  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / l->base.downsample,
                                      y / l->base.downsample,
                                      level, w, h, err);
}

/* ---------- associated images -------------------------------------------- */

static bool vmic_get_associated_image_data(
    struct _openslide_associated_image *_img,
    uint32_t *dest,
    GError **err) {
  struct zip_associated_image *img = (struct zip_associated_image *)_img;
  uint32_t *buf;
  int32_t w, h;

  if (!_openslide_zip_read_image(img->refzip, img->zipindex,
                                  IMAGE_FORMAT_JPEG, &buf, &w, &h, err)) {
    return false;
  }

  if (img->base.w != w || img->base.h != h) {
    g_free(buf);
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "VMIC associated image: size mismatch");
    return false;
  }

  memcpy(dest, buf, (size_t)w * h * 4);
  g_free(buf);
  return true;
}

static void vmic_destroy_associated_image(
    struct _openslide_associated_image *_img) {
  g_free(_img);
}

static const struct _openslide_associated_image_ops precipoint_associated_ops = {
  .get_argb_data = vmic_get_associated_image_data,
  .destroy = vmic_destroy_associated_image,
};

static void vmic_collect_associated_images(openslide_t *osr, GError **err) {
  struct vmicinfo *vmic = (struct vmicinfo *)osr->data;

  zip_int64_t file_id = _openslide_zip_name_locate(&vmic->zi,
                                                    _PRECIPOINT_MACRO_IMAGE,
                                                    ZIP_FL_ENC_RAW |
                                                    ZIP_FL_NOCASE);
  if (file_id <= 0) { return; }

  uint32_t *buf;
  int32_t w, h;
  if (!_openslide_zip_read_image(&vmic->zi, file_id, IMAGE_FORMAT_JPEG,
                                  &buf, &w, &h, err)) {
    return;
  }

  struct zip_associated_image *img = g_new0(struct zip_associated_image, 1);
  img->base.ops = &precipoint_associated_ops;
  img->base.w = w;
  img->base.h = h;
  img->refzip = &vmic->zi;
  img->zipindex = file_id;

  g_hash_table_insert(osr->associated_images, g_strdup("macro"), img);
  g_free(buf);
}

/* ---------- slide destroy ------------------------------------------------- */

static void vmic_destroy(openslide_t *osr) {
  struct vmicinfo *vmic = (struct vmicinfo *)osr->data;
  vmic_destroy_levels(osr);
  vmic_close_container(vmic);
  g_free(vmic);
}

/* ---------- ops / format entry points ------------------------------------ */

static const struct _openslide_ops precipoint_ops = {
  .paint_region = vmic_paint_region,
  .destroy = vmic_destroy,
};

static bool precipoint_detect(const char *filename,
                               struct _openslide_tifflike *tl,
                               GError **err) {
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  struct vmicinfo *vmic = vmic_open_container(filename, true, err);
  if (!vmic) { return false; }

  vmic_close_container(vmic);
  g_free(vmic);
  return true;
}

static bool precipoint_open(openslide_t *osr,
                             const char *filename,
                             struct _openslide_tifflike *tl G_GNUC_UNUSED,
                             struct _openslide_hash *quickhash1,
                             GError **err) {
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);

  struct vmicinfo *vmic = vmic_open_container(filename, false, err);
  if (!vmic) { return false; }

  osr->data = vmic;

  if (!vmic_get_properties(osr, quickhash1, err)) { goto FAIL; }
  if (!vmic_create_levels(osr, err)) { goto FAIL; }

  vmic_collect_associated_images(osr, err);
  if (*err) { goto FAIL; }

  /* include bottom-of-pyramid tile data in quickhash */
  gchar *hashfile = g_strdup_printf("%s/%d/0_0.%s",
                                    vmic->base.folder_name,
                                    vmic->base.dz_one_tile_level,
                                    vmic->base.tile_imgformat_str);
  int64_t zph = _openslide_zip_name_locate(&vmic->zi, hashfile,
                                            ZIP_FL_ENC_RAW | ZIP_FL_NOCASE);
  g_free(hashfile);

  if (zph == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "precipoint_open: quickhash tile not found");
    goto FAIL;
  }

  gpointer hashbuf;
  gsize hashsz;
  if (!_openslide_zip_read_file_data(&vmic->zi, zph,
                                      &hashbuf, &hashsz, err)) {
    g_prefix_error(err, "precipoint_open: cannot read quickhash tile: ");
    goto FAIL;
  }

  _openslide_hash_data(quickhash1, hashbuf, hashsz);
  g_free(hashbuf);

  osr->ops = &precipoint_ops;
  return true;

FAIL:
  vmic_close_container(vmic);
  g_free(vmic);
  osr->data = NULL;
  return false;
}

const struct _openslide_format _openslide_format_precipoint_vmic = {
  .name = _PRECIPOINT_VMICTYPE,
  .vendor = _PRECIPOINT_VENDOR,
  .detect = precipoint_detect,
  .open = precipoint_open,
};
