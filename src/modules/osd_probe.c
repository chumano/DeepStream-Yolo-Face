#include "osd_probe.h"
#include "config.h"
#include "nvbufsurface.h"
#include <time.h>
#include "utils.h"

GST_DEBUG_CATEGORY_EXTERN(deepstream_debug_category);
#define GST_CAT_DEFAULT deepstream_debug_category

// =============================================================================
// OSD Probe Helpers
// =============================================================================

static void
set_custom_bbox(NvDsObjectMeta *obj_meta, guint frame_width, uint frame_height)
{
  guint border_width = 1;
  guint font_size = 12;

  gfloat x_offset = obj_meta->rect_params.left - border_width * 0.5f;
  gfloat y_offset = obj_meta->rect_params.top - font_size * 2 + border_width * 0.5f + 1;

  // Set display text to show object ID and confidence
  g_snprintf(obj_meta->text_params.display_text, app_config.osd_text.max_display_len, "ID: %lu  %.2f", obj_meta->object_id, obj_meta->confidence);

  obj_meta->rect_params.border_width = border_width;
  obj_meta->rect_params.border_color.red = 0.0;
  obj_meta->rect_params.border_color.green = 0.0;
  obj_meta->rect_params.border_color.blue = 1.0;
  obj_meta->rect_params.border_color.alpha = 1.0;

  obj_meta->text_params.font_params.font_name = (gchar *) "Ubuntu";
  obj_meta->text_params.font_params.font_size = font_size;
  obj_meta->text_params.x_offset = (guint) MIN(frame_width - 1, MAX(0, x_offset));
  obj_meta->text_params.y_offset = (guint) MIN(frame_height - 1, MAX(0, y_offset));
  obj_meta->text_params.font_params.font_color.red = 1.0;
  obj_meta->text_params.font_params.font_color.green = 1.0;
  obj_meta->text_params.font_params.font_color.blue = 1.0;
  obj_meta->text_params.font_params.font_color.alpha = 1.0;
  obj_meta->text_params.set_bg_clr = 1;
  obj_meta->text_params.text_bg_clr.red = 0.0;
  obj_meta->text_params.text_bg_clr.green = 0.0;
  obj_meta->text_params.text_bg_clr.blue = 1.0;
  obj_meta->text_params.text_bg_clr.alpha = 1.0;
}

/**
 * Draw face landmark circles onto a display meta for the given object.
 * Landmarks are read from obj_meta->mask_params (packed as [x, y, conf] floats).
 * The display meta pool is acquired from batch_meta as needed.
 * *lm_display_meta is an in/out parameter: the caller passes its current
 * display-meta pointer (may be NULL) and receives the (possibly updated) one.
 */
static void
draw_landmark_circles(NvDsBatchMeta *batch_meta, NvDsFrameMeta *frame_meta,
                      NvDsObjectMeta *obj_meta, NvDsDisplayMeta **lm_display_meta, 
                      guint frame_width, uint frame_height)
{
  if (app_config.display.disabled)
    return;
  if (!obj_meta->mask_params.data || obj_meta->mask_params.size == 0)
    return;

  guint num_joints = obj_meta->mask_params.size / (sizeof(float) * 3);
  gfloat gain = MIN((gfloat)obj_meta->mask_params.width / frame_width,
                    (gfloat)obj_meta->mask_params.height / frame_height);
  gfloat pad_x = (obj_meta->mask_params.width  - frame_width  * gain) * 0.5f;
  gfloat pad_y = (obj_meta->mask_params.height - frame_height * gain) * 0.5f;

  for (guint i = 0; i < num_joints; ++i) {
    gfloat xc         = (obj_meta->mask_params.data[i * 3 + 0] - pad_x) / gain;
    gfloat yc         = (obj_meta->mask_params.data[i * 3 + 1] - pad_y) / gain;
    gfloat confidence =  obj_meta->mask_params.data[i * 3 + 2];

    if (confidence < 0.5f)
      continue;

    if (!*lm_display_meta || (*lm_display_meta)->num_circles == MAX_ELEMENTS_IN_DISPLAY_META) {
      *lm_display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
      nvds_add_display_meta_to_frame(frame_meta, *lm_display_meta);
    }

    NvOSD_CircleParams *cp = &(*lm_display_meta)->circle_params[(*lm_display_meta)->num_circles];
    cp->xc = (guint)MIN(frame_width  - 1, MAX(0, xc));
    cp->yc = (guint)MIN(frame_height - 1, MAX(0, yc));
    cp->radius = 6;
    cp->circle_color.red   = 1.0f;
    cp->circle_color.green = 1.0f;
    cp->circle_color.blue  = 1.0f;
    cp->circle_color.alpha = 1.0f;
    cp->has_bg_color = 1;
    cp->bg_color.red   = 0.0f;
    cp->bg_color.green = 0.0f;
    cp->bg_color.blue  = 1.0f;
    cp->bg_color.alpha = 1.0f;
    (*lm_display_meta)->num_circles++;
  }
}

/**
 * Add an NTP timestamp text overlay to the given frame using a display meta
 * acquired from batch_meta's pool. Does nothing if ntp_timestamp is zero.
 */
static void
add_ntp_timestamp_overlay(NvDsBatchMeta *batch_meta, NvDsFrameMeta *frame_meta)
{
  if (!frame_meta->ntp_timestamp)
    return;

  NvDsDisplayMeta *display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
  if (!display_meta)
    return;

  // Convert NTP timestamp (nanoseconds) to human-readable string
  gchar ntp_timestamp[64];
  format_ntp_timestamp(frame_meta->ntp_timestamp, ntp_timestamp, sizeof(ntp_timestamp));

  // 
  gchar full_timestamp[128];
  g_snprintf(full_timestamp, sizeof(full_timestamp),
             "FRAME %d, NTP: %s",
             frame_meta->frame_num, ntp_timestamp);

  // Configure text overlay parameters
  NvOSD_TextParams *txt_params  = &display_meta->text_params[0];
  display_meta->num_labels = 1;

  txt_params->display_text = g_strdup(full_timestamp);
  txt_params->x_offset     = app_config.osd_text.ntp_text_x_offset;
  txt_params->y_offset     = app_config.osd_text.ntp_text_y_offset;

  txt_params->font_params.font_name          = (gchar *)"Ubuntu";
  txt_params->font_params.font_size          = app_config.osd_text.ntp_text_font_size;
  txt_params->font_params.font_color.red     = 1.0f;
  txt_params->font_params.font_color.green   = 1.0f;
  txt_params->font_params.font_color.blue    = 1.0f;
  txt_params->font_params.font_color.alpha   = 1.0f;

  txt_params->set_bg_clr           = 1;
  txt_params->text_bg_clr.red      = 0.0f;
  txt_params->text_bg_clr.green    = 0.0f;
  txt_params->text_bg_clr.blue     = 0.0f;
  txt_params->text_bg_clr.alpha    = 0.7f;

  nvds_add_display_meta_to_frame(frame_meta, display_meta);
}

// =============================================================================
// OSD Sink Pad Probe
// =============================================================================

GstPadProbeReturn
nvosd_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  //wait 1 *1000ms for test
  //g_usleep(1000 * 1000);

  GstBuffer *buf = (GstBuffer *) info->data;
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);

  if (!batch_meta) {
    GST_ERROR("Failed to get batch meta");
    return GST_PAD_PROBE_OK;
  }

  // Get NvBufSurface from GstBuffer using the correct DeepStream method
  GstMapInfo map_info;
  if (!gst_buffer_map(buf, &map_info, GST_MAP_READ)) {
    GST_ERROR("Failed to map buffer");
    return GST_PAD_PROBE_OK;
  }

  NvBufSurface *surface = (NvBufSurface *)map_info.data;

  // Validate surface before use
  gboolean surface_valid = (surface != NULL &&
                            surface->numFilled > 0 &&
                            surface->surfaceList != NULL);

  if (surface_valid) {
    GST_DEBUG("Got valid surface: numFilled=%d, memType=%d",
            surface->numFilled, surface->memType);
  } else {
    GST_WARNING("Invalid or NULL surface");
    surface = NULL;
  }

  // Process each frame in batch
  NvDsMetaList *l_frame = NULL;
  for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);
    int mux_w = surface->surfaceList[frame_meta->batch_id].width; // = streammux width
    int mux_h = surface->surfaceList[frame_meta->batch_id].height;  // = streammux height
    LetterboxGeometry lb_geom = compute_letterbox_geometry(mux_w, mux_h, 
        frame_meta->source_frame_width, frame_meta->source_frame_height);

    // get frame meta timestamp (ntp_timestamp in ns, converted to seconds)
    gchar ntp_timestamp[64];
    format_ntp_timestamp(frame_meta->ntp_timestamp, ntp_timestamp, sizeof(ntp_timestamp));
    
    GstClockTime pts = frame_meta->buf_pts;

    GST_DEBUG("[osd] stream %d==%d, frame %d, org size [%d X %d],  streammux size [%d X %d], letterbox [%d X %d], pad [%d, %d], scale %.2f, PTS=%" GST_TIME_FORMAT ", NTP=%s\n",
            frame_meta->source_id,
            frame_meta->pad_index,
            frame_meta->frame_num,
            frame_meta->source_frame_width,
            frame_meta->source_frame_height,
            mux_w, 
            mux_h,
            lb_geom.content_w,
            lb_geom.content_h,
            lb_geom.pad_x,
            lb_geom.pad_y,
            lb_geom.scale,
            GST_TIME_ARGS(pts),
            ntp_timestamp
          );
    //  stream 0==0, source [1280 X 720], streammux size [640 X 640], letterbox [640 X 360], pad [0, 140], scale 0.50

    // Add NTP timestamp overlay (once per frame)
    add_ntp_timestamp_overlay(batch_meta, frame_meta);

    // Only draw if inference was done on this frame
    if (!frame_meta->bInferDone) {
      continue;
    }

    NvDsDisplayMeta *display_meta = NULL;
    NvDsMetaList *l_obj = NULL;
    for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) (l_obj->data);

      if (app_config.osd.draw_custom_bbox)
        set_custom_bbox(obj_meta, mux_w, mux_h);

      // Draw landmarks (circles) for display
      if (app_config.osd.draw_landmarks)
        draw_landmark_circles(batch_meta, frame_meta, obj_meta, &display_meta, mux_w, mux_h);

      // Free mask_params after processing
      // if (obj_meta->mask_params.data) {
      //   g_free(obj_meta->mask_params.data);
      //   obj_meta->mask_params.data = NULL;
      //   obj_meta->mask_params.width = 0;
      //   obj_meta->mask_params.height = 0;
      //   obj_meta->mask_params.size = 0;
      // }
    }

  }

  gst_buffer_unmap(buf, &map_info);

  return GST_PAD_PROBE_OK;
}
