#ifndef __IMAGE_PROCESSING_H__
#define __IMAGE_PROCESSING_H__

#include <glib.h>
#include "nvbufsurface.h"
#include "gstnvdsmeta.h"
#include "face.h"
#include "utils.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * Encode cropped region to base64 JPEG
 */
gchar *encode_crop_to_base64_jpeg(NvBufSurface *surface, CropBox *crop_box, 
                                   gint quality, guint batch_id);

/**
 * Ensure frame save directory exists
 */
gboolean ensure_frame_save_directory(const gchar *dir_path);

/**
 * Save frame to JPEG file and return relative path
 */
gchar *save_frame_to_jpeg(NvBufSurface *surface, NvDsFrameMeta *frame_meta,
                          const gchar *base_output_dir, gint quality, 
                          gboolean exclude_letterbox, LetterboxGeometry *lb_geom);

/**
 * Encode frame to JPEG in memory without writing to disk.
 * On success, *out_data is g_malloc()-allocated and *out_size is set.
 * Returns TRUE on success, FALSE on failure.
 * Caller must g_free() *out_data.
 */
gboolean save_frame_to_jpeg_mem(NvBufSurface *surface, NvDsFrameMeta *frame_meta,
                                gint quality, gboolean exclude_letterbox,
                                LetterboxGeometry *lb_geom,
                                guchar **out_data, gsize *out_size);

/**
 * Copy a single GPU surface slot to a newly allocated CPU RGBA buffer.
 * Optionally crops to the content area when exclude_letterbox=TRUE.
 * On success sets *out_width, *out_height, *out_pitch and returns a
 * g_malloc()-owned RGBA byte array (caller must g_free()).
 * Returns NULL on failure.
 *
 * Note: performs a GPU→GPU format/crop transform followed by a
 * cudaMemcpy; must be called on a thread that owns the surface.
 */
guchar *surface_slot_to_rgba_cpu(NvBufSurface *surface, guint batch_id,
                                  gboolean exclude_letterbox,
                                  LetterboxGeometry *lb_geom,
                                  guint *out_width, guint *out_height,
                                  guint *out_pitch);

/**
 * Encode a CPU-resident packed RGBA buffer (pitch-aligned rows) to JPEG
 * in memory.  On success, *out_data is g_malloc()-allocated and
 * *out_size is set.  Returns TRUE on success, FALSE on failure.
 * Caller must g_free() *out_data.
 */
gboolean encode_rgba_cpu_to_jpeg_mem(const guchar *rgba_data,
                                      guint width, guint height, guint pitch,
                                      gint quality,
                                      guchar **out_data, gsize *out_size);

#ifdef __cplusplus
}
#endif

#endif
