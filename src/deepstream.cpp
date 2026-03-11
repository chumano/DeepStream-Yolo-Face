#include "deepstream.h"
#include <jpeglib.h>
#include <sys/stat.h>
#include <string.h>
#include <gst-nvdssr.h>

//  modules
#include "modules/config.h"
#include "modules/face.h"
#include "modules/face_analysis.h"
#include "modules/file_cleanup.h"
#include "modules/image_processing.h"
#include "modules/json_builder.h"
#include "modules/osd_probe.h"
#include "modules/pipeline_builder.h"
#include "modules/pipeline_dump.h"
#include "modules/utils.h"

// GST_DEBUG_CATEGORY_STATIC to GST_DEBUG_CATEGORY  
//which makes the symbol externally visible so that osd_probe.c
GST_DEBUG_CATEGORY(deepstream_debug_category);
#define GST_CAT_DEFAULT deepstream_debug_category

// Per-source raw frame counter — incremented every time a raw frame arrives
// from the source tee appsink.  Access is single-threaded per source (each
// GStreamer pad-probe / appsink callback fires on one thread per source).
#define MAX_RAW_SOURCES 64
static guint   raw_src_frame_counters[MAX_RAW_SOURCES] = {0};
// Per-source smart-record session IDs — updated after each start-sr call so
// the assigned session ID is available for stop-sr or logging.
static guint32 sr_session_ids[MAX_RAW_SOURCES] = {0};

// =============================================================================
// Utility Functions
// =============================================================================

static gdouble
get_current_time(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (gdouble) ts.tv_sec + (gdouble) ts.tv_nsec / 1000000000.0;
}

// =============================================================================
// Raw source appsink callback (per-source, fires BEFORE nvstreammux)
// =============================================================================

/**
 * Called for every decoded-and-converted RGBA frame on the per-source tee
 * branch (before nvstreammux, so frames are at original source resolution
 * with no letterbox and no OSD overlays).
 *
 * @user_data  source index passed as GUINT_TO_POINTER(source_id)
 *
 * Behaviour depends on the configured frame-save mode:
 *   save_all_frames  → encode + write JPEG to disk immediately.
 *   otherwise        → encode + push into the per-source FrameBuffer ring;
 *                      the inference appsink flushes the ring to disk on
 *                      detection (frame_buffer_flush_to_disk).
 */
static GstFlowReturn
raw_src_appsink_callback(GstElement *appsink, gpointer user_data)
{
  guint source_id = GPOINTER_TO_UINT(user_data);

  if (!app_config.frame_save.enabled || !app_config.frame_save.dir)
    return GST_FLOW_OK;

  GstSample *sample = NULL;
  g_signal_emit_by_name(appsink, "pull-sample", &sample);
  if (!sample) {
    GST_ERROR("[raw-tee] Failed to pull sample (src=%u)", source_id);
    return GST_FLOW_ERROR;
  }

  GstBuffer *buf = gst_sample_get_buffer(sample);
  if (!buf) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  GstMapInfo map_info;
  if (!gst_buffer_map(buf, &map_info, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  NvBufSurface *surface = (NvBufSurface *)map_info.data;
  if (!surface || surface->numFilled == 0 || !surface->surfaceList) {
    gst_buffer_unmap(buf, &map_info);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  /* Buffer PTS as stream timestamp (ns → seconds); fall back to wall clock */
  GstClockTime pts = GST_BUFFER_PTS(buf);
  GstClockTime dts = GST_BUFFER_DTS(buf);

  // get ntp_timestamp from buffer metadata if available (ns → seconds) : available on gststreamer version 1.22+
  GstReferenceTimestampMeta *meta = (GstReferenceTimestampMeta *)gst_buffer_get_meta(buf, gst_reference_timestamp_meta_api_get_type());

  GstClockTime ntp = (meta) ? meta->timestamp : GST_CLOCK_TIME_NONE;

  GST_DEBUG("[raw-tee] src=%u frame=%u PTS=%" GST_TIME_FORMAT " DTS=%" GST_TIME_FORMAT " NTP=%" GST_TIME_FORMAT "\n",
          source_id, raw_src_frame_counters[source_id] + 1, 
          GST_TIME_ARGS(pts), 
          GST_TIME_ARGS(dts),
          GST_TIME_ARGS(ntp));


  // Convert to seconds with fallback
  gdouble timestamp = (pts != GST_CLOCK_TIME_NONE)
                      ? (gdouble)pts / 1e9
                      : get_current_time();

  /* Per-source monotonic frame counter */
  guint frame_num = 0;
  if (source_id < MAX_RAW_SOURCES)
    frame_num = ++raw_src_frame_counters[source_id];

  /* Record source-level frame arrival and check for PTS gaps (network/decoder drops) */
  if (pipeline_monitor) {
    pipeline_monitor_record_source_frame(pipeline_monitor, source_id, pts,
                                         200.0 /* ms gap threshold ≈ 5 dropped frames @25fps */);
  }

  /*
   * The raw surface is NOT batched (single frame, batch_id = 0).
   * No letterbox exists here — save the full frame.
   */
  NvDsFrameMeta fm_local = {};
  fm_local.batch_id  = 0;
  fm_local.source_id = source_id;
  fm_local.frame_num = frame_num;

  /* Letterbox geometry with no padding (full source resolution) */
  LetterboxGeometry lb_noop = {};
  lb_noop.content_w = surface->surfaceList[0].width;
  lb_noop.content_h = surface->surfaceList[0].height;
  lb_noop.scale     = 1.0f;

  if (app_config.frame_save.save_all_frames) {
    /* ── Mode 1: save every raw frame directly to disk ── */
    gchar *rel = save_frame_to_jpeg(surface, &fm_local,
                                    app_config.frame_save.dir,
                                    app_config.frame_save.quality,
                                    FALSE, &lb_noop);
    GST_DEBUG("[raw-tee/save-all] src=%u frame=%u -> %s",
              source_id, frame_num, rel ? rel : "NULL");
    g_free(rel);

  } else if (frame_buffer) {
    /* ── Mode 2 / smart: push into ring buffer; flush on detection ── */
    guchar *jpeg_data = NULL;
    gsize   jpeg_size = 0;
    if (save_frame_to_jpeg_mem(surface, &fm_local,
                               app_config.frame_save.quality,
                               FALSE, &lb_noop,
                               &jpeg_data, &jpeg_size)) {
      frame_buffer_push(frame_buffer, source_id, frame_num,
                        timestamp, jpeg_data, jpeg_size);
      frame_buffer_prune(frame_buffer, source_id, timestamp);
      GST_TRACE("[raw-tee/buf] src=%u frame=%u ts=%.3f",
                source_id, frame_num, timestamp);
    }
  }

  gst_buffer_unmap(buf, &map_info);
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}


// =============================================================================
// Smart Record callback
// =============================================================================

/**
 * Called by nvurisrcbin when a smart-recording session finishes writing.
 *
 * @user_data  source index passed as GUINT_TO_POINTER(stream_id)
 *
 * Logs the completed recording and prints a visible confirmation line.
 * Extend here to trigger post-processing, move the file, or publish a
 * notification.
 */
static void
sr_done_callback(GstElement *src, NvDsSRRecordingInfo *info, gpointer user_data)
{
  guint stream_id = GPOINTER_TO_UINT(user_data);

  if (!info) {
    GST_WARNING("[smart-record] sr-done fired for src=%u but info is NULL", stream_id);
    return;
  }

  gchar *full_path = (info->dirpath && info->filename)
                     ? g_strdup_printf("%s/%s", info->dirpath, info->filename)
                     : g_strdup(info->filename ? info->filename : "(unknown)");

  gdouble duration_sec = (gdouble)info->duration / 1000.0;

  GST_INFO("[smart-record] Recording done: src=%u sessionId=%u file=%s "
           "duration=%.2fs container=%u %ux%u",
           stream_id,
           info->sessionId,
           full_path,
           duration_sec,
           info->containerType,
           info->width, info->height);

  g_print("[smart-record] Saved: %s (%.2f s)\n", full_path, duration_sec);

  /* Send smart-record completion event to Kafka if enabled */
  if (app_config.kafka.enabled && detection_manager &&
      detection_manager_is_enabled(detection_manager)) {

    gchar *json = build_smart_record_event_json(
        stream_id,
        info->sessionId,
        full_path,
        duration_sec,
        info->containerType,
        info->width,
        info->height,
        get_current_time());

    GST_INFO("[smart-record] Sending Kafka event: %s", json);

    if (!detection_manager_send_event(detection_manager, "smart_record_done", json)) {
      GST_WARNING("[smart-record] Failed to send Kafka event "
                  "for src=%u sessionId=%u", stream_id, info->sessionId);
    }

    g_free(json);
  }

  g_free(full_path);
}

// =============================================================================
// Face Processing Pipeline
// =============================================================================

/**
 * Calculate crop box with padding around bounding box
 */
void
calculate_crop_box(NvDsObjectMeta *obj_meta, CropBox *crop_box, 
  guint frame_width, guint frame_height)
{
  // Add 20% padding around the bounding box
  gfloat padding = 0.2f;
  
  gfloat pad_w = obj_meta->rect_params.width * padding;
  gfloat pad_h = obj_meta->rect_params.height * padding;
  
  gint left = (gint)(obj_meta->rect_params.left - pad_w);
  gint top = (gint)(obj_meta->rect_params.top - pad_h);
  gint right = (gint)(obj_meta->rect_params.left + obj_meta->rect_params.width + pad_w);
  gint bottom = (gint)(obj_meta->rect_params.top + obj_meta->rect_params.height + pad_h);
  
  // Clamp to frame boundaries
  crop_box->left = MAX(0, left);
  crop_box->top = MAX(0, top);
  crop_box->width = MIN(right, (gint)frame_width) - crop_box->left;
  crop_box->height = MIN(bottom, (gint)frame_height) - crop_box->top;
}

/**
 * Process a single detected object/face.
 * Returns a newly-allocated JSON string describing the detection, or NULL if
 * the object was filtered out or JSON building is not required.
 * The caller is responsible for g_free()-ing the returned string.
 */
static gchar *
process_object(NvDsFrameMeta *frame_meta, NvDsObjectMeta *obj_meta, NvBufSurface *surface,
    gchar* frame_image_path, LetterboxGeometry *lb_geom)
{
  gdouble frame_timestamp = 0.0;
  if (frame_meta && frame_meta->ntp_timestamp) {
    frame_timestamp = (gdouble)frame_meta->ntp_timestamp / 1e9;
  } else {
    GST_WARNING("Frame meta or ntp_timestamp is NULL, using current time");
    frame_timestamp = get_current_time();
  }

  guint frame_width = surface->surfaceList[frame_meta->batch_id].width; // = streammux width
  guint frame_height = surface->surfaceList[frame_meta->batch_id].height; // = streammux height



  // Extract landmarks from object metadata
  guint num_landmarks = 0;
  Landmark *landmarks = extract_landmarks_from_object(obj_meta, &num_landmarks, frame_width, frame_height);
  if (!landmarks || num_landmarks < 5) {
    GST_DEBUG("Insufficient landmarks (%u) for object_id=%lu", 
              num_landmarks, obj_meta->object_id);
    g_free(landmarks);
    return NULL;
  }

  // Assess face quality
  gboolean is_good_face = FALSE;
  gdouble quality_score = 0.0;
  FaceQualityMetrics metrics = {0};

  if (!assess_face_quality(landmarks, num_landmarks, 
                           &is_good_face, &quality_score, &metrics)) {
    GST_DEBUG("Failed to assess face quality for object_id=%lu", obj_meta->object_id);
    g_free(landmarks);
    return NULL;
  }

  GST_DEBUG("Face quality for object_id=%lu: is_good=%s, score=%.3f",
            obj_meta->object_id, is_good_face ? "true" : "false", quality_score);


  // Encode cropped face image if enabled
  CropBox crop_box;
  calculate_crop_box(obj_meta, &crop_box, frame_width, frame_height);
  gchar *face_image_base64 = NULL;
  if (app_config.enable_crop_image && surface) {
    face_image_base64 = encode_crop_to_base64_jpeg(surface, &crop_box, 85, frame_meta->batch_id);
  }

  // Bbox
  CropBox bbox = {
    .left = (guint) obj_meta->rect_params.left,
    .top = (guint) obj_meta->rect_params.top,
    .width = (guint) obj_meta->rect_params.width,
    .height = (guint) obj_meta->rect_params.height
  };

  // Adjust coordinates if letterbox is present and we want to exclude letterbox area from saved image
  if(app_config.frame_save.exclude_letterbox) {
    // Recalculate Bounding Box Coordinates
    // Adjust bbox and crop_box coordinates to account for letterbox padding
    bbox.left -= lb_geom->pad_x;
    bbox.top -= lb_geom->pad_y;

    crop_box.left -= lb_geom->pad_x;
    crop_box.top -= lb_geom->pad_y;

    // Adjust landmark coordinates as well
    for (guint i = 0; i < num_landmarks; i++) {
      landmarks[i].x -= lb_geom->pad_x;
      landmarks[i].y -= lb_geom->pad_y;
    }

  }

  // Create face context
  FaceContext ctx = {
    .source_id = frame_meta->source_id,
    .frame_timestamp = frame_timestamp,
    .frame_num = (guint) frame_meta->frame_num,
    .object_id = obj_meta->object_id,
    .class_id = obj_meta->class_id,
    .confidence = obj_meta->confidence,
    .frame_width = frame_width,
    .frame_height = frame_height,

    //
    .landmarks = landmarks,
    .num_landmarks = num_landmarks,
    .frame_image_path = frame_image_path,
    //
    .bbox = &bbox,
    .crop_box = &crop_box,
    .is_good_face = is_good_face,
    .quality_score = quality_score,
    .metrics = &metrics,
    .face_image_base64 = face_image_base64,
  };

  // Record detection metrics
  if (pipeline_monitor) {
    pipeline_monitor_record_detection(pipeline_monitor, ctx.source_id,
                                      ctx.is_good_face, ctx.quality_score);
  }

  // Build detection JSON (used for Kafka and/or JSON file output)
  gchar *json_data = NULL;
  bool need_json = (app_config.json_save.enabled) ||
                   (app_config.kafka.enabled && detection_manager &&
                    detection_manager_is_enabled(detection_manager));
  if (need_json) {
    json_data = build_detection_json(&ctx);
  }

  // Process face detection and send to Kafka
  if (app_config.kafka.enabled && detection_manager && detection_manager_is_enabled(detection_manager)) {
      detection_manager_queue(detection_manager, ctx.source_id, ctx.object_id,
                              ctx.quality_score, json_data);
  }

  // Cleanup
  g_free(face_image_base64);
  g_free(landmarks);

  // Return JSON string to caller (caller must g_free; may be NULL)
  return json_data;
}

/**
 * Process a single object from the secondary (traffic) inference engine.
 * Returns a newly-allocated JSON string, or NULL on error.
 * The caller is responsible for g_free()-ing the returned string.
 */
static gchar *
process_traffic_object(NvDsFrameMeta *frame_meta, NvDsObjectMeta *obj_meta,
                       const gchar *frame_image_path)
{
  gdouble frame_timestamp = 0.0;
  if (frame_meta && frame_meta->ntp_timestamp) {
    frame_timestamp = (gdouble)frame_meta->ntp_timestamp / 1e9;
  } else {
    frame_timestamp = get_current_time();
  }

  if (pipeline_monitor) {
    pipeline_monitor_record_detection(pipeline_monitor, frame_meta->source_id,
                                      FALSE, (gdouble)obj_meta->confidence);
  }

  return build_generic_object_json(
      frame_meta->source_id,
      (guint)frame_meta->frame_num,
      frame_timestamp,
      obj_meta->object_id,
      obj_meta->class_id,
      obj_meta->obj_label,
      (gdouble)obj_meta->confidence,
      (guint)obj_meta->rect_params.left,
      (guint)obj_meta->rect_params.top,
      (guint)obj_meta->rect_params.width,
      (guint)obj_meta->rect_params.height,
      obj_meta->unique_component_id);
}

/**
 * Write a frame-level JSON file to disk.
 * File path: {json_save.dir}/source_{source_id}/frame_{frame_num:06d}.json
 */
static void
save_frame_to_json(guint source_id, guint frame_num, gdouble timestamp,
                   const gchar *frame_image_path,
                   gchar **object_jsons, guint num_objects)
{
  if (!app_config.json_save.enabled || !app_config.json_save.dir)
    return;

  // Ensure per-source subdirectory exists
  gchar *src_dir = g_strdup_printf("%s/source_%u",
                                    app_config.json_save.dir, source_id);
  g_mkdir_with_parents(src_dir, 0755);

  gchar *file_path = g_strdup_printf("%s/frame_%06u.json", src_dir, frame_num);
  g_free(src_dir);

  gchar *json = build_frame_json(source_id, frame_num, timestamp,
                                  frame_image_path, object_jsons, num_objects);
  if (json) {
    GError *err = NULL;
    g_file_set_contents(file_path, json, -1, &err);
    if (err) {
      GST_WARNING("Failed to write frame JSON to %s: %s", file_path, err->message);
      g_error_free(err);
    } else {
      GST_DEBUG("Saved frame JSON: %s", file_path);
    }
    g_free(json);
  }
  g_free(file_path);
}


static GstFlowReturn
appsink_new_sample_callback(GstElement *appsink, gpointer user_data)
{
  AppPipeline *ap = (AppPipeline *)user_data;
  GstSample *sample = NULL;
  
  // Pull sample from appsink
  g_signal_emit_by_name(appsink, "pull-sample", &sample);
  
  if (!sample) {
    GST_ERROR("Failed to pull sample from appsink");
    return GST_FLOW_ERROR;
  }
  
  // Get buffer from sample
  GstBuffer *buf = gst_sample_get_buffer(sample);
  if (!buf) {
    GST_ERROR("Failed to get buffer from sample");
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }
  
  // Get batch metadata
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
  if (!batch_meta) {
    GST_ERROR("Failed to get batch meta");
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }
  
  // Map buffer to get surface
  GstMapInfo map_info;
  if (!gst_buffer_map(buf, &map_info, GST_MAP_READ)) {
    GST_ERROR("Failed to map buffer");
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }
  
  NvBufSurface *surface = (NvBufSurface *)map_info.data;
  
  // get memtype of surface for debugging
  if(surface) {
    GST_DEBUG("Got surface from appsink: numFilled=%d, memType=%d", 
              surface->numFilled, surface->memType);
  }

  // Validate surface
  gboolean surface_valid = (surface != NULL && 
                            surface->numFilled > 0 && 
                            surface->surfaceList != NULL);
  
  if (!surface_valid) {
    GST_WARNING("Invalid surface");
    gst_buffer_unmap(buf, &map_info);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }
  
  // Process each frame in batch
  NvDsMetaList *l_frame = NULL;
  for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);
    LetterboxGeometry lb_geom = compute_letterbox_geometry(
      surface->surfaceList[frame_meta->batch_id].width,
      surface->surfaceList[frame_meta->batch_id].height,
      frame_meta->source_frame_width,
      frame_meta->source_frame_height
    );

    // === Record pipeline metrics (detection count, latency) ===
    // Record frame-level metrics and latency
    gboolean has_objects = (frame_meta->obj_meta_list != NULL);
    if (pipeline_monitor) {
      pipeline_monitor_record_frame(pipeline_monitor,
                                    frame_meta->source_id,
                                    (gboolean)frame_meta->bInferDone,
                                    has_objects);

      // Compute end-to-end latency: buffer PTS (ns) vs. wall clock
      if (frame_meta->ntp_timestamp > 0) {
        gdouble buf_time_sec  = (gdouble)frame_meta->ntp_timestamp / 1e9;
        gdouble wall_time_now = (gdouble)g_get_real_time() / 1e6; /* µs -> ms already? no */
        // g_get_real_time returns microseconds
        wall_time_now = (gdouble)g_get_real_time() / 1e6; /* us -> ms */
        buf_time_sec  = buf_time_sec * 1e3;               /* s -> ms */
        gdouble latency_ms = wall_time_now - buf_time_sec;
        if (latency_ms > 0.0 && latency_ms < 60000.0) {
          pipeline_monitor_record_latency(pipeline_monitor, latency_ms);
        }
      }
    }

    // === Save frame to disk if enabled ===
    // check frame is infer done
    if (frame_meta->bInferDone == FALSE) {
      GST_TRACE("Frame %d inference not done yet", frame_meta->frame_num);
      continue;
    }

    GST_DEBUG("Processing frame %d with %d objects",
              frame_meta->frame_num, frame_meta->num_obj_meta);

    gchar *image_rel_path = NULL;
    // Save frame to disk if enabled
    if (app_config.frame_save.enabled && surface_valid && app_config.frame_save.dir) {
     
      //if (frame_meta->frame_num % FRAME_SAVE_INTERVAL == 0) {
      // Check if frame has any detected objects (faces)
      // This variable holds the count for the current frame
      if (  frame_meta->obj_meta_list != NULL 
        && frame_meta->num_obj_meta > 0 ) {
        image_rel_path = save_frame_to_jpeg(surface, frame_meta,
                          app_config.frame_save.dir, app_config.frame_save.quality,
                          app_config.frame_save.exclude_letterbox,
                          &lb_geom);
        GST_DEBUG("Saved frame %d to %s",
                  frame_meta->frame_num,
                  image_rel_path ? image_rel_path : "NULL");
      }
      //}
    }

    // Raw frames are captured in raw_src_appsink_callback from the per-source
    // tee branch before nvstreammux (original resolution, no letterbox, no OSD).
    // For pre-buffer and smart modes, flush the ring buffer on detection.
    if (app_config.frame_save.enabled
        && !app_config.frame_save.save_all_frames
        && frame_buffer
        && frame_meta->obj_meta_list != NULL
        && frame_meta->num_obj_meta > 0) {
      GstClockTime pts = frame_meta->buf_pts;
      gdouble detection_pts_sec = (gdouble) pts / GST_SECOND;
                                    
      GST_INFO("[detection] Detection on src=%u frame=%u: flushed pre-buffer pts=%f",
                frame_meta->source_id, frame_meta->frame_num, detection_pts_sec);
      
      // trigger save frame
      frame_buffer_save_frame(frame_buffer, frame_meta->source_id, detection_pts_sec);
    }

    // Trigger nvurisrcbin Smart Record on detection
    // https://docs.nvidia.com/metropolis/deepstream/7.1/text/DS_Smart_video.html
    if (app_config.smart_record.enabled
        && ap
        && frame_meta->obj_meta_list != NULL
        && frame_meta->num_obj_meta > 0) {
      guint src_id = frame_meta->source_id;
      if (src_id < ap->num_sources && ap->src_bins[src_id]) {
        GstState cur_state = GST_STATE_NULL;
        gst_element_get_state(ap->src_bins[src_id], &cur_state, NULL, 0);
        // Only trigger smart record when the source bin is actually playing.
        // During RTSP reconnect the bin is in READY/PAUSED and emitting
        // start-sr on it can corrupt internal state or crash.

         if (cur_state == GST_STATE_PLAYING) {
          /* start-sr(sessionId, start_time=cache_size, duration=0, file_path=NULL)
          * start_time: how many seconds of cache to include before the trigger.
          * duration=0: record until stop-sr is sent (auto-stop by default-duration). */
          guint start_time = app_config.smart_record.cache_size_sec;
          guint duration   = 0;   /* 0 → auto-stop after default-duration */
          guint32 sessId   = 0;   /* output: filled by nvurisrcbin start-sr */
          g_signal_emit_by_name(ap->src_bins[src_id], "start-sr",
                                &sessId, start_time, duration, 2);
          /* Store the assigned session ID so it can be referenced later
           * (e.g. for stop-sr, deduplication, or Kafka events). */
          if (src_id < MAX_RAW_SOURCES)
            sr_session_ids[src_id] = sessId;
          GST_DEBUG("[smart-record] start-sr triggered for src=%u frame=%u, sessionId=%u",
                  src_id, frame_meta->frame_num, sessId);
        }else {
          GST_WARNING("[smart-record] skip start-sr for src=%u: state=%s (reconnecting?)",
                   src_id, gst_element_state_get_name(cur_state));
        }
      }
    }

    // Process each object in frame
    GPtrArray *obj_jsons = g_ptr_array_new_with_free_func(g_free);
    NvDsMetaList *l_obj = NULL;
    for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *)(l_obj->data);

      gchar *obj_json = NULL;

      if (obj_meta->unique_component_id == 2) {
        // Secondary inference (traffic / infer2) — no landmarks, generic JSON
        obj_json = process_traffic_object(frame_meta, obj_meta, image_rel_path);
      } else {
        // Primary inference (face / infer) — full face pipeline
        obj_json = process_object(frame_meta, obj_meta, surface, image_rel_path, 
                    &lb_geom);
      }

      if (obj_json)
        g_ptr_array_add(obj_jsons, obj_json);
    }

    // Write per-frame JSON to disk if enabled (only when objects were detected)
    if (app_config.json_save.enabled && app_config.json_save.dir && obj_jsons->len > 0) {
      gdouble frame_ts = frame_meta->ntp_timestamp
                         ? (gdouble)frame_meta->ntp_timestamp / 1e9
                         : get_current_time();
      save_frame_to_json(frame_meta->source_id,
                         (guint)frame_meta->frame_num,
                         frame_ts,
                         image_rel_path,
                         (gchar **)obj_jsons->pdata,
                         obj_jsons->len);
    }

    g_ptr_array_free(obj_jsons, TRUE);

    if(image_rel_path) {
      g_free(image_rel_path);
    }

  }
  
  // Cleanup
  gst_buffer_unmap(buf, &map_info);
  gst_sample_unref(sample);
  
  return GST_FLOW_OK;
}

// =============================================================================
// Detection Manager Callback
// =============================================================================

static gboolean
detection_manager_process_pending_callback(gpointer user_data)
{
  if (detection_manager) {
    detection_manager_process_pending(detection_manager);
  }
  return TRUE;
}

static void
init_detection_manager(void)
{
  if (!app_config.kafka.enabled) {
    detection_manager = detection_manager_new(FALSE, NULL, NULL, NULL,
                                             app_config.kafka.send_delay_sec,
                                             app_config.kafka.quality_improvement_threshold,
                                             app_config.kafka.cleanup_interval_sec,
                                             app_config.kafka.sent_record_ttl_sec,
                                             app_config.kafka.pending_ttl_sec);
    return;
  }
  
  detection_manager = detection_manager_new(TRUE, app_config.kafka.broker, app_config.kafka.topic,
                                           app_config.kafka.event_topic,
                                           app_config.kafka.send_delay_sec,
                                           app_config.kafka.quality_improvement_threshold,
                                           app_config.kafka.cleanup_interval_sec,
                                           app_config.kafka.sent_record_ttl_sec,
                                           app_config.kafka.pending_ttl_sec);
  
  if (!detection_manager_init_kafka(detection_manager)) {
    GST_WARNING("WARNING - Failed to initialize Kafka, running without Kafka\n");
  }
  
  GST_DEBUG("Detection manager initialized (Kafka: %s, Topic: %s)\n",
          app_config.kafka.broker, app_config.kafka.topic);
}


static void
cleanup_detection_manager(void)
{
  if (detection_manager) {
    detection_manager_print_stats(detection_manager);
    detection_manager_cleanup_kafka(detection_manager);
    detection_manager_free(detection_manager);
    detection_manager = NULL;
  }
}

// =============================================================================
// Main Function
// =============================================================================

static void
log_parsed_config(void)
{
  GST_INFO("");
  GST_INFO("DEBUG - After parsing:");
  GST_INFO("  NUM_SOURCES: %d", app_config.source.count);
  if (app_config.source.uris) {
    for (guint i = 0; i < app_config.source.count; i++) {
      GST_INFO("  SOURCES[%d]: %s", i, app_config.source.uris[i]);
    }
  } else {
    GST_INFO("  SOURCES: (null)");
  }
  GST_INFO("INFER_CONFIG: %s", app_config.infer.config_file);
  GST_INFO("GPU_ID: %d",               app_config.gpu_id);
  GST_INFO("PERF_MEASUREMENT_INTERVAL_SEC: %d", app_config.perf_measurement_interval_sec);
  GST_INFO("JETSON: %s",     app_config.jetson      ? "TRUE" : "FALSE");
  GST_INFO("USE_TRITON: %s", app_config.infer.use_triton ? "TRUE" : "FALSE");
  if (app_config.kafka.enabled) {
    GST_INFO("KAFKA_BROKER: %s",                         app_config.kafka.broker);
    GST_INFO("KAFKA_TOPIC: %s",                          app_config.kafka.topic);
    GST_INFO("KAFKA_SEND_DELAY_SEC: %.1f",               app_config.kafka.send_delay_sec);
    GST_INFO("KAFKA_QUALITY_IMPROVEMENT_THRESHOLD: %.2f",app_config.kafka.quality_improvement_threshold);
  }
  GST_INFO("ENABLE_CROP_IMAGE: %s", app_config.enable_crop_image ? "TRUE" : "FALSE");
  if (app_config.frame_save.enabled) {
    GST_INFO("FRAME_SAVE_DIR: %s",     app_config.frame_save.dir);
    GST_INFO("FRAME_SAVE_QUALITY: %u", app_config.frame_save.quality);
  }
  GST_INFO("");
}

gint
main(gint argc, char *argv[])
{
  gint ret = 0;
  GMainLoop *loop = NULL;
  AppPipeline *ap = NULL;
  gint current_device = -1;
  struct cudaDeviceProp prop;
  const gchar *dump_dir = "/app/outputs";

  // Initialize GStreamer and GST debug category before any GST_* logging
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(deepstream_debug_category, "deepstream", 0, "DeepStream Face App");

  // ============================================================================
  // Parse command-line options
  if (!parse_command_line(argc, argv)) {
    g_printerr("ERROR - Failed to parse command-line options\n");
    ret = -1;
    goto cleanup;
  }
   
  // Check if running on Jetson by querying CUDA device properties
  cudaGetDevice(&current_device);
  cudaGetDeviceProperties(&prop, current_device);
  if (prop.integrated) {
    app_config.jetson = TRUE;
  }

  //================================================
  // Debug: Print what was actually parsed
  log_parsed_config();

  // ============================================================================
  // wait user to press enter key to start
  if (app_config.wait_for_user_input) {
    g_print("Press ENTER to start processing ...\n");
    getchar();
  }
 
  // ============================================================================
  // Initialize frame save directory if enabled
  if (app_config.frame_save.enabled) {
    if (!app_config.frame_save.dir) {
      app_config.frame_save.dir = g_strdup("/app/outputs/frames");
    }
    
    if (!ensure_frame_save_directory(app_config.frame_save.dir)) {
      g_printerr("ERROR - Failed to create frame save directory: %s\n", app_config.frame_save.dir);
      ret = -1;
      goto cleanup;
    }
    
    GST_INFO("Frame saving enabled: dir=%s, quality=%u", 
            app_config.frame_save.dir, app_config.frame_save.quality);
  }


  // ============================================================================
  // Initialize pipeline monitor
  GST_INFO("Initializing pipeline monitor...");
  pipeline_monitor = pipeline_monitor_new(app_config.perf_measurement_interval_sec, app_config.source.count);
  if (!pipeline_monitor) {
    g_printerr("WARNING - Failed to create pipeline monitor, continuing without metrics\n");
  }

  // ============================================================================
  // Initialize detection manager
  GST_INFO("Initializing detection manager...");
  init_detection_manager();

  // ============================================================================
  // Initialize file cleanup timer (deletes old frames/videos)
  GST_INFO("Initializing file cleanup...");
  file_cleanup_start();

  // ============================================================================
  // Initialize raw-frame ring buffer (used for pre-buffer and smart-save modes)
  // Not needed when save_all_frames is true because every frame is written
  // directly to disk by raw_src_appsink_callback.
  if (app_config.frame_save.enabled 
      && !app_config.frame_save.save_all_frames) {
    GST_INFO("Initializing raw-frame ring buffer (%.2fs window, %u source(s))...",
             app_config.frame_save.pre_buffer_duration_sec, app_config.source.count);
    frame_buffer = frame_buffer_new(app_config.source.count,
                                    app_config.frame_save.pre_buffer_duration_sec,
                                    app_config.frame_save.dir);
    if (!frame_buffer) {
      g_printerr("WARNING - Failed to create frame buffer, buffered saving disabled\n");
    }
  }

  loop = g_main_loop_new(NULL, FALSE);

  _intr_setup();
  g_timeout_add(400, check_for_interrupt, &loop);

  // Start periodic check for pending detections
  if (app_config.kafka.enabled && detection_manager) {
    g_timeout_add(500, detection_manager_process_pending_callback, NULL);
  }

  // ============================================================================
  // Create GStreamer pipeline
  GST_INFO("Creating GStreamer pipeline...");
  ap = create_app_pipeline(
      loop,
      G_CALLBACK(appsink_new_sample_callback),
      G_CALLBACK(sr_done_callback),
      pipeline_monitor);
  if (!ap) {
    g_printerr("ERROR - Failed to create pipeline\n");
    ret = -1;
    goto cleanup;
  }

  // ============================================================================
  // Connect per-source raw appsink callbacks (tee branch before nvstreammux)
  if (app_config.frame_save.enabled && ap->src_appsinks) {
    for (guint i = 0; i < ap->num_sources; i++) {
      if (ap->src_appsinks[i]) {
        g_signal_connect(ap->src_appsinks[i], "new-sample",
                         G_CALLBACK(raw_src_appsink_callback),
                         GUINT_TO_POINTER(i));
        GST_INFO("Connected raw appsink for source %u", i);
      }
    }
  }

  // ===============================================
  // Start the pipeline
  // Go directly to PLAYING — do NOT call GST_STATE_PAUSED first.
  // For live sources (RTSP + nvstreammux) an explicit PAUSED transition forces
  // a preroll that can block indefinitely when sources are not yet ready.
  // GStreamer automatically traverses NULL→READY→PAUSED→PLAYING internally.
  GST_INFO("Starting GStreamer pipeline...\n");
  {
    GstStateChangeReturn sc_ret =
        gst_element_set_state(ap->pipeline, GST_STATE_PLAYING);
    if (sc_ret == GST_STATE_CHANGE_FAILURE) {
      g_printerr("ERROR - Failed to set pipeline to playing\n");
      ret = -1;
      goto cleanup;
    }
    if (sc_ret == GST_STATE_CHANGE_ASYNC) {
      GST_INFO("Pipeline state change is async (normal for live sources)\n");
    }
  }

  /* Dump all pipeline elements + properties to a JSON file for inspection */
  dump_pipeline_elements_to_json(ap->pipeline, dump_dir);

  GST_DEBUG("\n");

  g_main_loop_run(loop);
  g_print("\nPipeline stopped, performing cleanup...\n");

  gst_element_set_state(ap->pipeline, GST_STATE_NULL);

  // ===============================================
  // Print final pipeline metrics report
  if (pipeline_monitor) {
    g_print("\n=== FINAL PIPELINE METRICS REPORT ===\n");
    pipeline_monitor_print_report(pipeline_monitor);
  }

cleanup:
  if (pipeline_monitor) {
    pipeline_monitor_free(pipeline_monitor);
    pipeline_monitor = NULL;
  }

  // Cleanup detection manager
  cleanup_detection_manager();

  // Stop file cleanup timer
  file_cleanup_stop();

  // Cleanup pre-detection frame buffer
  if (frame_buffer) {
    frame_buffer_free(frame_buffer);
    frame_buffer = NULL;
  }

  config_free();
  if (ap) destroy_app_pipeline(ap);
  if (loop) g_main_loop_unref(loop);

  return ret;
}
