/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2022-2024 Benjamin Gilbert
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
 * CSP format (csp) support
 */

#include "openslide-decode-jpeg.h"
#include "openslide-private.h"

#include <glib.h>
#include <stdint.h>

struct csp_scan_config {
  uint32_t tile_width; // CSP min tile width, default 256*256 (all pyramid tiles
                       // use this size)
  uint32_t tile_height;
  uint32_t image_width; // max scan scale image width (level 0 size)
  uint32_t image_height;
  float scan_ratio;         // max scan magnification (e.g. 40x, 20x)
  float mpp;                // physical size per pixel (insert to hash table)
  float downsampling_ratio; // downsampling ratio: calc pyramid layer scan mag
  char scan_time[128];      // slide scan time
};                          // basic config info of CSP file

enum csp_associated_image_type {
  ASSOCIATED_IMAGE_TYPE_LABEL = 0,
  ASSOCIATED_IMAGE_TYPE_PREVIEW = 1,
  ASSOCIATED_IMAGE_TYPE_THUMBNAIL = 2,
};

struct csp_associated_image_info {
  enum csp_associated_image_type image_type;
  uint32_t width;
  uint32_t height;
  uint64_t data_offset;
  uint64_t data_length;
};

struct csp_jpeg_info {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
  uint64_t data_offset; // tile offset based on pixel_file_offset (add to seek
                        // offset when reading file)
  uint32_t data_length;
  uint32_t crc32;
};

struct csp_frame_info {
  uint32_t frame_id;
  uint32_t frame_width;
  uint32_t frame_height;
  float frame_ratio;
  GPtrArray *multi_tile; // csp_jpeg_info type: all tiles per pyramid layer
                         // (release after use)
};

struct csp_image_info {
  uint32_t image_id;
  GPtrArray *multi_frame; // csp_frame_info type: each element maps to a pyramid
                          // layer (release after use)
};

struct csp_scan_result {
  struct csp_scan_config config;
  struct csp_image_info multi_image;
};

struct csp_file {
  _openslide_file *filehandle;
  uint32_t offset_type; // addressing mode (param, get when parsing CSP header;
                        // fill before use, required for entry header parsing)
  uint64_t parsed_offset;     // parsed data offset of CSP format (update after
                              // parsing each entry)
  uint64_t pixel_file_offset; // start offset of pixel data (offset based on
                              // this when reading tile)
  char manufacturer[64];      // scanner manufacturer
  char model_name[64];        // scanner model
  struct csp_associated_image_info
      associated_images[3]; // store associated images
  struct csp_scan_result scan_result;
};

static const int OFF_TYPE_U16 = 16;
static const int OFF_TYPE_U32 = 32;

/* scanner */
static const uint32_t UID_EQUIPMENT = 0x00010001;
static const uint32_t UID_MANUFACTURER = 0x00010002;
static const uint32_t UID_MODEL_NAME = 0x00010003;
// static const uint32_t UID_SERIAL_NUMBER = 0x00010004;
// static const uint32_t UID_SOFTWARE_VERSION = 0x00010005;
/* associated image */
static const uint32_t UID_ASSOCIATED_IMAGE = 0x00020001;
static const uint32_t UID_IMAGE_TYPE = 0x00020002;
static const uint32_t UID_IMAGE_WIDTH = 0x00020003;
static const uint32_t UID_IMAGE_HEIGHT = 0x00020004;
static const uint32_t UID_IMAGE_DATA_OFFSET = 0x00020005;
static const uint32_t UID_IMAGE_DATA_LENGTH = 0x00020006;
// static const uint32_t UID_BARCODE_VALUE = 0x00020007;
// static const uint32_t UID_IMAGE_REMARKS = 0x00020008;
/* pixel data */
static const uint32_t UID_PIXEL_DATA = 0x00030001;
/* multi scan result */
static const uint32_t UID_MULTI_SCAN_RESULT = 0x00050001;
/* scan result */
static const uint32_t UID_SCAN_RESULT = 0x00050002;
/* scan configuration */
static const uint32_t UID_SCAN_CONFIGURATION = 0x00040001;
static const uint32_t UID_SCAN_TIME = 0x00040003;
// static const uint32_t UID_SCAN_DURATION = 0x00040004;
static const uint32_t UID_SLICE_BASIC_WIDTH = 0x00040007;
static const uint32_t UID_SLICE_BASIC_HEIGHT = 0x00040008;
static const uint32_t UID_SCAN_RATIO = 0x00040009;
static const uint32_t UID_SCAN_MPP = 0x0004000A;
static const uint32_t UID_DOWNSAMPLING_RATIO = 0x00060002;
/* multi image info */
static const uint32_t UID_MULTI_IMAGE_INFO = 0x00020009;
static const uint32_t UID_IMAGE_INFO = 0x0002000A;
static const uint32_t UID_IMAGE_ID = 0x0002000B;
/* metadata of multi-frame */
static const uint32_t UID_MULTI_FRAME_INFO = 0x0002001E;
/* metadata of specific frame */
static const uint32_t UID_FRAME_INFO = 0x0002001F;
static const uint32_t UID_FRMAE_ID = 0x00020020;
static const uint32_t UID_FRMAE_RATIO = 0x00020021;
static const uint32_t UID_FRAME_WIDTH = 0x00020022;
static const uint32_t UID_FRAME_HEIGHT = 0x00020023;
/* multi tile info */
static const uint32_t UID_MULTI_TILE_INFO = 0x00020024;
/* specific tile info */
static const uint32_t UID_TILE_INFO = 0x00020025;

/******** value of DT ********/
static const int DT_SEQUENCE = 0x000e;

struct csp_data_entry_node {
  uint32_t uid;
  uint16_t dt;
  uint64_t value_num;
  uint64_t value_length;
  char *value;
  GPtrArray *children;
};

static uint16_t bytes_to_uint16(const char *bytes) {
  uint16_t addr = (uint16_t)(bytes[0]) & 0xFF;
  addr |= (((uint16_t)(bytes[1]) << 8) & 0xFF00);
  return addr;
}

static uint32_t bytes_to_uint32(const char *bytes) {
  uint32_t addr = (uint32_t)(bytes[0]) & 0xFF;
  addr |= (((uint32_t)(bytes[1]) << 8) & 0xFF00);
  addr |= (((uint32_t)(bytes[2]) << 16) & 0xFF0000);
  addr |= (((uint32_t)(bytes[3]) << 24) & 0xFF000000);
  return addr;
}

static uint64_t bytes_to_uint64(const char *bytes) {
  uint64_t addr = (uint64_t)(bytes[0]) & 0xFF;
  addr |= (((uint64_t)(bytes[1]) << 8) & 0xFF00);
  addr |= (((uint64_t)(bytes[2]) << 16) & 0xFF0000);
  addr |= (((uint64_t)(bytes[3]) << 24) & 0xFF000000);
  addr |= (((uint64_t)(bytes[4]) << 32) & 0xFF00000000);
  addr |= (((uint64_t)(bytes[5]) << 40) & 0xFF0000000000);
  addr |= (((uint64_t)(bytes[6]) << 48) & 0xFF000000000000);
  addr |= (((uint64_t)(bytes[7]) << 56) & 0xff00000000000000);
  return addr;
}

static int32_t parse_equipment(struct csp_data_entry_node *entry,
                               struct csp_file *file) {
  if (entry->dt != DT_SEQUENCE) {
    return -1;
  }
  for (uint32_t i = 0; i < entry->children->len; i++) {
    struct csp_data_entry_node *child =
        (struct csp_data_entry_node *)entry->children->pdata[i];
    switch (child->uid) {
    case UID_MANUFACTURER:
      memcpy(file->manufacturer, child->value, child->value_length);
      break;
    case UID_MODEL_NAME:
      memcpy(file->model_name, child->value, child->value_length);
      break;
    default:
      break;
    }
  }
  return 0;
}

static int32_t parse_tile_info(struct csp_data_entry_node *entry,
                               struct csp_jpeg_info *tile_info) {
  char *buf = entry->value;
  uint32_t offset = 0;
  uint32_t width = bytes_to_uint32(buf + offset);
  offset += sizeof(uint32_t);
  uint32_t height = bytes_to_uint32(buf + offset);
  offset += sizeof(uint32_t);
  tile_info->data_offset = bytes_to_uint64(buf + offset);
  offset += sizeof(uint64_t);
  tile_info->data_length = bytes_to_uint64(buf + offset);
  offset += sizeof(uint64_t);
  tile_info->x = bytes_to_uint32(buf + offset);
  offset += sizeof(uint32_t);
  tile_info->y = bytes_to_uint32(buf + offset);
  offset += sizeof(uint32_t);
  tile_info->crc32 = bytes_to_uint32(buf + offset);
  offset += sizeof(uint32_t);
  tile_info->width = width;
  tile_info->height = height;

  return 0;
}

static void csp_tile_free(struct csp_jpeg_info *tile_info) {
  g_free(tile_info);
}
OPENSLIDE_DEFINE_G_DESTROY_NOTIFY_WRAPPER(csp_tile_free)

static int32_t parse_multi_tiles(struct csp_data_entry_node *entry,
                                 struct csp_frame_info *frame_info) {
  int32_t ret = 0;
  frame_info->multi_tile = g_ptr_array_new_with_free_func(
      OPENSLIDE_G_DESTROY_NOTIFY_WRAPPER(csp_tile_free));
  for (uint32_t i = 0; i < entry->children->len; i++) {
    struct csp_data_entry_node *child =
        (struct csp_data_entry_node *)entry->children->pdata[i];
    if (child->uid != UID_TILE_INFO) {
      return -1;
    }
    struct csp_jpeg_info *tile_info = g_new0(struct csp_jpeg_info, 1);
    g_ptr_array_add(frame_info->multi_tile, tile_info);
    ret = parse_tile_info(child, tile_info);
    if (ret != 0) {
      return ret;
    }
  }
  g_ptr_array_unref(entry->children);

  return 0;
}

// parse information of a single frame (maps to a single pyramid layer)
static int32_t parse_frame_info(struct csp_data_entry_node *entry,
                                struct csp_frame_info *frame) {
  int32_t ret = 0;
  for (uint32_t i = 0; i < entry->children->len; i++) {
    struct csp_data_entry_node *child =
        (struct csp_data_entry_node *)entry->children->pdata[i];
    switch (child->uid) {
    case UID_FRMAE_ID:
      frame->frame_id = bytes_to_uint32(child->value);
      break;
    case UID_FRMAE_RATIO:
      frame->frame_ratio = *(float *)(child->value);
      break;
    case UID_FRAME_WIDTH:
      frame->frame_width = bytes_to_uint32(child->value);
      break;
    case UID_FRAME_HEIGHT:
      frame->frame_height = bytes_to_uint32(child->value);
      break;
    case UID_MULTI_TILE_INFO:
      ret = parse_multi_tiles(child, frame);
      if (ret != 0) {
        g_ptr_array_unref(entry->children);
        return ret;
      }
      break;
    default:
      break;
    }
  }
  g_ptr_array_unref(entry->children);

  return ret;
}

static void frame_free(struct csp_frame_info *frame) { g_free(frame); }
OPENSLIDE_DEFINE_G_DESTROY_NOTIFY_WRAPPER(frame_free)

static int32_t parse_multi_frame_info(struct csp_data_entry_node *entry,
                                      GPtrArray **multi_frame) {
  int32_t ret = 0;

  *multi_frame = g_ptr_array_new_with_free_func(
      OPENSLIDE_G_DESTROY_NOTIFY_WRAPPER(frame_free));
  for (uint32_t i = 0; i < entry->children->len; i++) {
    struct csp_data_entry_node *child =
        (struct csp_data_entry_node *)entry->children->pdata[i];
    if (child->uid != UID_FRAME_INFO) {
      g_ptr_array_unref(*multi_frame);
      *multi_frame = NULL;
      return -1;
    }
    struct csp_frame_info *frame = g_new0(struct csp_frame_info, 1);
    g_ptr_array_add(*multi_frame, frame);
    ret = parse_frame_info(child, frame);
    if (ret != 0) {
      g_ptr_array_unref(*multi_frame);
      *multi_frame = NULL;
      return -1;
    }
  }
  g_ptr_array_unref(entry->children);
  return ret;
}

static int32_t parse_image_info(struct csp_data_entry_node *entry,
                                struct csp_image_info *image_info) {
  int32_t ret = 0;
  for (uint32_t i = 0; i < entry->children->len; i++) {
    struct csp_data_entry_node *child =
        (struct csp_data_entry_node *)entry->children->pdata[i];
    switch (child->uid) {
    case UID_IMAGE_ID:
      image_info->image_id = bytes_to_uint32(child->value);
      break;
    case UID_MULTI_FRAME_INFO:
      ret = parse_multi_frame_info(child, &(image_info->multi_frame));
      break;
    default:
      break;
    }
    if (ret != 0) {
      g_ptr_array_unref(entry->children);
      return ret;
    }
  }

  return 0;
}

// parse scan configuration info
static void parse_scan_config_part(struct csp_data_entry_node *entry,
                                   struct csp_scan_config *config) {
  switch (entry->uid) {
  case UID_SCAN_TIME:
    memcpy(config->scan_time, entry->value, entry->value_length);
    break;
  case UID_SLICE_BASIC_WIDTH:
    config->tile_width = bytes_to_uint32(entry->value);
    break;
  case UID_SLICE_BASIC_HEIGHT:
    config->tile_height = bytes_to_uint32(entry->value);
    break;
  case UID_SCAN_RATIO:
    config->scan_ratio = *(float *)(entry->value);
    break;
  case UID_SCAN_MPP:
    config->mpp = *(float *)(entry->value);
    break;
  case UID_DOWNSAMPLING_RATIO:
    config->downsampling_ratio = *(float *)(entry->value);
    break;
  default:
    break;
  }
  return;
}

static int32_t parse_scan_config(struct csp_data_entry_node *entry,
                                 struct csp_scan_config *config) {

  for (uint32_t i = 0; i < entry->children->len; i++) {
    struct csp_data_entry_node *child =
        (struct csp_data_entry_node *)entry->children->pdata[i];
    parse_scan_config_part(child, config);
  }
  if (config->tile_width == 0) {
    g_ptr_array_unref(entry->children);
    return -1;
  }
  g_ptr_array_unref(entry->children);

  return 0;
}

static int32_t parse_multi_image_info(struct csp_data_entry_node *entry,
                                      struct csp_image_info *multi_image) {
  if (entry->children->len > 1) {
    // only one image layer is present (not supported here, intercept directly)
    g_ptr_array_unref(entry->children);
    return -1;
  }
  int32_t ret = 0;
  struct csp_data_entry_node *child =
      (struct csp_data_entry_node *)entry->children->pdata[0];
  if (child->uid != UID_IMAGE_INFO) {
    g_ptr_array_unref(entry->children);
    return -1;
  }
  ret = parse_image_info(child, multi_image);
  if (ret != 0) {
    g_ptr_array_unref(entry->children);
    return ret;
  }
  g_ptr_array_unref(entry->children);

  return 0;
}

static int32_t parse_scan_result(struct csp_data_entry_node *entry,
                                 struct csp_scan_result *scan_result) {
  int32_t ret = 0;
  if (entry->dt != DT_SEQUENCE || entry->children->len > 2) {
    // multi-layer images are a v2.0 feature (only one layer exists in practice,
    // unsupported here, intercept directly)
    return -1;
  }

  for (uint32_t i = 0; i < entry->children->len; i++) {
    struct csp_data_entry_node *child =
        (struct csp_data_entry_node *)entry->children->pdata[i];
    if (child->uid == UID_SCAN_CONFIGURATION) {
      ret = parse_scan_config(child, &scan_result->config);
      if (ret != 0) {
        g_ptr_array_unref(entry->children);
        return ret;
      }
      continue;
    }
    if (child->uid == UID_MULTI_IMAGE_INFO) {
      ret = parse_multi_image_info(child, &scan_result->multi_image);
      if (ret != 0) {
        g_ptr_array_unref(entry->children);
        return ret;
      }
      continue;
    }
  }
  g_ptr_array_unref(entry->children);

  return 0;
}

static int32_t
parse_multi_scan_result(struct csp_data_entry_node *entry,
                        struct csp_scan_result *multi_scan_result) {
  int32_t ret = 0;
  if (entry->dt != DT_SEQUENCE || entry->children->len > 1) {
    // multiple scans are a reserved feature (only one scan file is generated by
    // current CSP; intercept multiple scans directly)
    g_ptr_array_unref(entry->children);
    return -1;
  }
  struct csp_data_entry_node *child =
      (struct csp_data_entry_node *)entry->children->pdata[0];
  if (child->uid != UID_SCAN_RESULT) {
    g_ptr_array_unref(entry->children);
    return -1;
  }
  ret = parse_scan_result(child, multi_scan_result);
  if (ret != 0) {
    g_ptr_array_unref(entry->children);
    return ret;
  }
  return ret;
}

static int32_t parse_add_associated_image_entry(
    struct csp_data_entry_node *entry,
    struct csp_associated_image_info *associated_image) {
  if (entry->dt != DT_SEQUENCE) {
    return -1;
  }
  struct csp_associated_image_info image;
  for (uint32_t i = 0; i < entry->children->len; i++) {
    struct csp_data_entry_node *child =
        (struct csp_data_entry_node *)entry->children->pdata[i];
    switch (child->uid) {
    case UID_IMAGE_TYPE:
      image.image_type = (enum csp_associated_image_type)(
          child->value[0]); // 0 - label image, 1 - macro image, 2 - thumbnail
      break;
    case UID_IMAGE_WIDTH:
      image.width = bytes_to_uint32(child->value);
      break;
    case UID_IMAGE_HEIGHT:
      image.height = bytes_to_uint32(child->value);
      break;
    case UID_IMAGE_DATA_OFFSET:
      image.data_offset = bytes_to_uint64(
          child->value); // offset relative to pixel (not file-based offset)
      break;
    case UID_IMAGE_DATA_LENGTH:
      image.data_length = bytes_to_uint64(child->value);
      break;
    default:
      break;
    }
  }
  memcpy(&associated_image[image.image_type], &image,
         sizeof(struct csp_associated_image_info));

  return 0;
}

static int32_t _openslide_parse_entry_node(struct csp_data_entry_node *entry,
                                           struct csp_file *file,
                                           GError **err) {
  int32_t ret = 0;
  switch (entry->uid) {
  case UID_EQUIPMENT:
    ret = parse_equipment(entry, file);
    break;
  case UID_ASSOCIATED_IMAGE:
    ret = parse_add_associated_image_entry(entry, file->associated_images);
    break;
  case UID_MULTI_SCAN_RESULT:
    ret = parse_multi_scan_result(entry, &file->scan_result);
    break;
  default:
    break;
  }

  if (ret != 0) {
    g_prefix_error(err, "parse entry(%d) failed!", entry->uid);
    g_message("_openslide_parse_entry_node (%x) failed!", entry->uid);
  }

  if (entry->uid == UID_PIXEL_DATA) {
    return ret;
  }

  g_ptr_array_unref(entry->children);
  return ret;
}

static uint32_t _openslide_get_de_hdr_size(uint32_t offset_type) {
  uint32_t ret = sizeof(uint32_t) + sizeof(uint16_t);
  if (offset_type == OFF_TYPE_U32) {
    ret += sizeof(uint32_t) + sizeof(uint32_t);
  } else if (offset_type == OFF_TYPE_U16) {
    ret += sizeof(uint16_t) + sizeof(uint16_t);
  } else {
    ret += sizeof(uint64_t) + sizeof(uint64_t);
  }

  return ret;
}

static uint32_t deserialize_hdr(uint32_t offset_type, char *metabuf,
                                struct csp_data_entry_node *entry) {
  uint32_t idx = 0;
  uint16_t mid = bytes_to_uint16(metabuf + idx);
  idx += sizeof(uint16_t);
  uint16_t eid = bytes_to_uint16(metabuf + idx);
  idx += sizeof(uint16_t);
  entry->uid = (uint32_t)(mid << 16) + eid;
  entry->dt = bytes_to_uint16(metabuf + idx);
  idx += sizeof(uint16_t);
  if (offset_type == OFF_TYPE_U32) {
    entry->value_num = bytes_to_uint32(metabuf + idx);
    idx += sizeof(uint32_t);
    entry->value_length = bytes_to_uint32(metabuf + idx);
    idx += sizeof(uint32_t);
  } else if (offset_type == OFF_TYPE_U16) {
    entry->value_num = bytes_to_uint16(metabuf + idx);
    idx += sizeof(uint16_t);
    entry->value_length = bytes_to_uint16(metabuf + idx);
    idx += sizeof(uint16_t);
  } else {
    entry->value_num = bytes_to_uint64(metabuf + idx);
    idx += sizeof(uint64_t);
    entry->value_length = bytes_to_uint64(metabuf + idx);
    idx += sizeof(uint64_t);
  }

  return idx;
}

static void children_free(struct csp_data_entry_node *child) { g_free(child); }
OPENSLIDE_DEFINE_G_DESTROY_NOTIFY_WRAPPER(children_free)

static int32_t _openslide_build_entry_node(struct csp_data_entry_node *entry,
                                           struct csp_file *file,
                                           GError **err) {
  if (entry->dt !=
      DT_SEQUENCE) { // non-SQ: no child nodes exist, skip further processing
    return 0;
  }
  uint32_t idx = 0;
  int32_t ret = 0;
  entry->children = g_ptr_array_new_with_free_func(
      OPENSLIDE_G_DESTROY_NOTIFY_WRAPPER(children_free));
  for (uint32_t i = 0; i < entry->value_num; ++i) {
    struct csp_data_entry_node *child = g_new0(struct csp_data_entry_node, 1);
    g_ptr_array_add(entry->children, child);
    uint32_t len =
        deserialize_hdr(file->offset_type, entry->value + idx, child);
    idx += len;
    child->value =
        entry->value + idx; // value of child node: no need to release (directly
                            // uses parent node's memory)
    if (idx + child->value_length >
        entry->value_length) { // insufficient remaining capacity for
                               // child->value
      g_ptr_array_unref(entry->children);
      g_prefix_error(err, "value left is not enough!");
      return -1;
    }
    ret = _openslide_build_entry_node(
        child, file, err); // recursively traverse all child entries
    if (ret != 0) {
      g_ptr_array_unref(entry->children);
      g_prefix_error(err, "build failed!");
      return -1;
    }
    idx += child->value_length;
  }
  if (idx != entry->value_length) {
    g_ptr_array_unref(entry->children);
    g_prefix_error(err, "value is bigger");
    return -1;
  }
  return 0;
}

static int32_t _openslide_read_entry_node(struct csp_data_entry_node *entry,
                                          struct csp_file *file, GError **err) {
  int32_t ret;
  uint64_t readLen;
  readLen = _openslide_get_de_hdr_size(file->offset_type);
  char buf[readLen];
  if (_openslide_fread(file->filehandle, buf, readLen, err) != readLen) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read file");
    return -1;
  }
  file->parsed_offset += readLen;
  deserialize_hdr(file->offset_type, buf,
                  entry); // parse each field of the entry
  readLen = entry->value_length;
  if (entry->uid == UID_PIXEL_DATA) {
    file->pixel_file_offset =
        file->parsed_offset; // only need to record the offset of pixel
  } else {
    entry->value = g_new0(char, readLen); // released after parsing
    if (_openslide_fread(file->filehandle, entry->value, readLen, err) !=
        readLen) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read file");
      return -1;
    }
    ret = _openslide_build_entry_node(entry, file, err);
    if (ret != 0) {
      g_prefix_error(err, "uild entry node failed[%d]!", ret);
      return -1;
    }
  }
  file->parsed_offset += readLen;
  if (!_openslide_fseek(file->filehandle, file->parsed_offset, SEEK_SET, err)) {
    g_prefix_error(err, "seek failed!");
    return -1;
  }

  return 0;
}

// entry point for CSP format parsing
static bool _openslide_decode_csp_file(struct csp_file *file, GError **err) {
  if (!_openslide_fseek(file->filehandle, 128, SEEK_SET,
                        err)) { // parse data starting from byte 128 onwards
    g_prefix_error(err, "Couldn't seek");
    return false;
  }
  file->parsed_offset = 128;
  uint64_t filesize = _openslide_fsize(file->filehandle, err);
  if (filesize <= 0) {
    g_prefix_error(err, "get size failed");
    return false;
  }

  int32_t ret = 0;
  do {
    struct csp_data_entry_node entry;
    ret = _openslide_read_entry_node(&entry, file, err);
    if (ret != 0) {
      g_free(entry.value);
      g_prefix_error(err, "read entry failed");
      return false;
    }
    ret = _openslide_parse_entry_node(&entry, file, err);
    if (ret != 0) {
      g_free(entry.value);
      g_prefix_error(err, "prase entry failed");
      return false;
    }

    if (entry.uid == UID_PIXEL_DATA) {
      continue;
    }
    g_free(entry.value);
  } while (file->parsed_offset < filesize);

  return true;
}

static const char CSP_EXT[] = ".csp";

struct csp_ops_data {
  char *filename;
};

struct image {
  int64_t start_in_file;
  int32_t length;
  int32_t imageno; // used only for cache lookup
  int32_t width;
  int32_t height;
  int refcount;
};

struct tile {
  struct image *image;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_free(l);
}
OPENSLIDE_DEFINE_G_DESTROY_NOTIFY_WRAPPER(destroy_level)

typedef struct level level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(level, destroy_level)

static void destroy(openslide_t *osr) {
  struct csp_ops_data *data = osr->data;

  // levels
  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *)osr->levels[i]);
  }
  g_free(osr->levels);

  // the ops data
  g_free(data->filename);
  g_free(data);
}

static void image_unref(struct image *image) {
  if (!--image->refcount) {
    g_free(image);
  }
}

typedef struct image image;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(image, image_unref)

static void tile_free(gpointer data) {
  struct tile *tile = data;
  image_unref(tile->image);
  g_free(tile);
}

static uint32_t *read_image(openslide_t *osr, struct image *image, int w, int h,
                            GError **err) {
  struct csp_ops_data *data = osr->data;
  bool result = false;

  g_autofree uint32_t *dest = g_malloc(w * h * 4);

  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
  if (f == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "File is NULL");
    return NULL;
  }

  if (image->length == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "Length is zero");
    return NULL;
  }

  if (image->start_in_file &&
      !_openslide_fseek(f, image->start_in_file, SEEK_SET, err)) {
    g_prefix_error(err, "Cannot seek to offset: ");
    return NULL;
  }

  char buf[image->length];
  if (_openslide_fread(f, buf, sizeof(buf), err) != sizeof(buf)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read tile data");
    return NULL;
  }

  result = _openslide_jpeg_decode_buffer(buf, image->length, dest, w, h, err);

  if (!result) {
    return NULL;
  }
  return g_steal_pointer(&dest);
}

static bool read_tile(openslide_t *osr, cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col G_GNUC_UNUSED,
                      int64_t tile_row G_GNUC_UNUSED, void *data,
                      void *arg G_GNUC_UNUSED, GError **err) {
  struct tile *tile = data;
  bool success = true;

  int iw = tile->image->width;
  int ih = tile->image->height;

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(
      osr->cache, level, tile->image->imageno, 0, &cache_entry);

  if (!tiledata) {
    tiledata = read_image(osr, tile->image, iw, ih, err);
    if (tiledata == NULL) {
      return false;
    }
    _openslide_cache_put(osr->cache, level, tile->image->imageno, 0, tiledata,
                         iw * ih * 4, &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface = cairo_image_surface_create_for_data(
      (unsigned char *)tiledata, CAIRO_FORMAT_RGB24, iw, ih, iw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return success;
}

static bool paint_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr, int64_t x,
                         int64_t y, struct _openslide_level *level, int32_t w,
                         int32_t h, GError **err) {
  struct level *l = (struct level *)level;

  return _openslide_grid_paint_region(l->grid, cr, NULL, x / level->downsample,
                                      y / level->downsample, level, w, h, err);
}

static const struct _openslide_ops csp_ops = {
    .paint_region = paint_region,
    .destroy = destroy,
};

static bool csp_detect(const char *filename G_GNUC_UNUSED,
                       struct _openslide_tifflike *tl, GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "Is a TIFF file");
    return false;
  }

  // verify filename
  if (!g_str_has_suffix(filename, CSP_EXT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s extension", CSP_EXT);
    return false;
  }

  // verify existence
  GError *tmp_err = NULL;
  if (!_openslide_fexists(filename, &tmp_err)) {
    if (tmp_err != NULL) {
      g_propagate_prefixed_error(err, tmp_err, "Testing whether file exists: ");
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "File does not exist");
    }
    return false;
  }

  return true;
}

static bool csp_open(openslide_t *osr, const char *filename,
                     struct _openslide_tifflike *tl G_GNUC_UNUSED,
                     struct _openslide_hash *quickhash1, GError **err) {
  // open file
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  char buf[128];
  if (_openslide_fread(f, buf, 128, err) != 128) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read file");
    return -1;
  }

  struct csp_file file;
  file.offset_type = bytes_to_uint16(buf + 12);
  file.filehandle = f;
  bool ret = _openslide_decode_csp_file(&file, err);

  struct csp_scan_result *scan_result = &(file.scan_result);
  struct csp_image_info *img_info = &(scan_result->multi_image);
  GPtrArray *multi_frame = img_info->multi_frame;

  // add properties
  g_hash_table_insert(osr->properties, g_strdup("csp.ScanTime"),
                      g_strdup(file.scan_result.config.scan_time));
  g_hash_table_insert(osr->properties, g_strdup("csp.ScanManufacturer"),
                      g_strdup(file.manufacturer));
  g_hash_table_insert(osr->properties, g_strdup("csp.ModelName"),
                      g_strdup(file.model_name));

  g_hash_table_insert(
      osr->properties, g_strdup(OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER),
      _openslide_format_double(file.scan_result.config.scan_ratio));
  g_hash_table_insert(osr->properties, g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                      _openslide_format_double(file.scan_result.config.mpp));
  g_hash_table_insert(osr->properties, g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                      _openslide_format_double(file.scan_result.config.mpp));

  if (file.associated_images[ASSOCIATED_IMAGE_TYPE_LABEL].data_offset) {
    _openslide_jpeg_add_associated_image(
        osr, "label", filename,
        file.pixel_file_offset +
            file.associated_images[ASSOCIATED_IMAGE_TYPE_LABEL].data_offset,
        err);
  }
  if (file.associated_images[ASSOCIATED_IMAGE_TYPE_PREVIEW].data_offset) {
    _openslide_jpeg_add_associated_image(
        osr, "macro", filename,
        file.pixel_file_offset +
            file.associated_images[ASSOCIATED_IMAGE_TYPE_PREVIEW].data_offset,
        err);
  }
  if (file.associated_images[ASSOCIATED_IMAGE_TYPE_THUMBNAIL].data_offset) {
    _openslide_jpeg_add_associated_image(
        osr, "thumbnail", filename,
        file.pixel_file_offset +
            file.associated_images[ASSOCIATED_IMAGE_TYPE_THUMBNAIL].data_offset,
        err);
  }

  osr->level_count = file.scan_result.multi_image.multi_frame->len;

  // set up level dimensions and such
  g_autoptr(GPtrArray) level_array = g_ptr_array_new_with_free_func(
      OPENSLIDE_G_DESTROY_NOTIFY_WRAPPER(destroy_level));
  int32_t image_number = 0;
  int64_t downsample = 1;
  for (int i = 0; i < osr->level_count; i++) {
    if (i == 0)
      downsample = 1;
    else
      downsample *= 2;

    struct csp_frame_info *current_frame =
        (struct csp_frame_info *)g_ptr_array_index(multi_frame, i);
    struct csp_jpeg_info *first_tile =
        (struct csp_jpeg_info *)g_ptr_array_index(current_frame->multi_tile, 0);

    struct level *l = g_new0(struct level, 1);
    g_ptr_array_add(level_array, l);

    l->base.downsample = downsample;
    l->base.tile_w = (double)first_tile->width;
    l->base.tile_h = (double)first_tile->height;

    l->base.w = current_frame->frame_width;
    l->base.h = current_frame->frame_height;

    l->grid = _openslide_grid_create_tilemap(
        osr, l->base.tile_w, l->base.tile_h, read_tile, tile_free);

    // insert tiles
    for (uint32_t t = 0; t < current_frame->multi_tile->len; t++) {
      struct csp_jpeg_info *tile_info =
          (struct csp_jpeg_info *)g_ptr_array_index(current_frame->multi_tile,
                                                    t);

      g_autoptr(image) image = g_new0(struct image, 1);
      image->start_in_file = file.pixel_file_offset + tile_info->data_offset;
      image->length = tile_info->data_length;
      image->imageno = image_number++;
      image->refcount = 2;
      image->width = tile_info->width;
      image->height = tile_info->height;

      struct tile *tile = g_new0(struct tile, 1);
      tile->image = image;
      _openslide_grid_tilemap_add_tile(l->grid, tile_info->x / tile_info->width,
                                       tile_info->y / tile_info->height, 0, 0,
                                       tile_info->width, tile_info->height,
                                       tile);
    }
  }

  // build ops data
  struct csp_ops_data *data = g_new0(struct csp_ops_data, 1);
  data->filename = g_strdup(filename);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **)g_ptr_array_free(
      g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &csp_ops;

  return ret;
}

const struct _openslide_format _openslide_format_csp = {
    .name = "csp",
    .vendor = "csp",
    .detect = csp_detect,
    .open = csp_open,
};
