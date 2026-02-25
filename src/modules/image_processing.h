#ifndef __IMAGE_PROCESSING_H__
#define __IMAGE_PROCESSING_H__

#include <glib.h>
#include "nvbufsurface.h"
#include "gstnvdsmeta.h"
#include "face.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * Calculate crop box with padding around bounding box
 */
void calculate_crop_box(NvDsObjectMeta *obj_meta, CropBox *crop_box, 
                        guint frame_width, guint frame_height);

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
                          const gchar *base_output_dir, gint quality);

#ifdef __cplusplus
}
#endif

#endif
