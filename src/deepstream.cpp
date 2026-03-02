#include "deepstream.h"
#include <jpeglib.h>
#include <sys/stat.h>

//  modules
#include "modules/config.h"
#include "modules/face.h"
#include "modules/face_analysis.h"
#include "modules/image_processing.h"
#include "modules/json_builder.h"

GST_DEBUG_CATEGORY_STATIC(deepstream_debug_category);
#define GST_CAT_DEFAULT deepstream_debug_category

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
// Face Processing Pipeline
// =============================================================================

/**
 * Process a single detected object/face.
 * Returns a newly-allocated JSON string describing the detection, or NULL if
 * the object was filtered out or JSON building is not required.
 * The caller is responsible for g_free()-ing the returned string.
 */
static gchar *
process_object(NvDsFrameMeta *frame_meta, NvDsObjectMeta *obj_meta, NvBufSurface *surface,
    gchar* frame_image_path)
{
  gdouble frame_timestamp = 0.0;
  if (frame_meta && frame_meta->ntp_timestamp) {
    frame_timestamp = (gdouble)frame_meta->ntp_timestamp / 1e9;
  } else {
    GST_WARNING("Frame meta or ntp_timestamp is NULL, using current time");
    frame_timestamp = get_current_time();
  }

  // Bbox
  CropBox bbox = {
    .left = (guint) obj_meta->rect_params.left,
    .top = (guint) obj_meta->rect_params.top,
    .width = (guint) obj_meta->rect_params.width,
    .height = (guint) obj_meta->rect_params.height
  };

  // Extract landmarks from object metadata
  guint num_landmarks = 0;
  Landmark *landmarks = extract_landmarks_from_object(obj_meta, &num_landmarks);
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
    return NULL;
  }

  GST_DEBUG("Face quality for object_id=%lu: is_good=%s, score=%.3f",
            obj_meta->object_id, is_good_face ? "true" : "false", quality_score);

  // Encode cropped face image if enabled
  CropBox crop_box;
  calculate_crop_box(obj_meta, &crop_box, app_config.streammux.width, app_config.streammux.height);
  gchar *face_image_base64 = NULL;
  if (app_config.enable_crop_image && surface) {
    face_image_base64 = encode_crop_to_base64_jpeg(surface, &crop_box, 85, frame_meta->batch_id);
  }

  // Create face context
  FaceContext ctx = {
    .source_id = frame_meta->source_id,
    .frame_timestamp = frame_timestamp,
    .frame_num = (guint) frame_meta->frame_num,
    .object_id = obj_meta->object_id,
    .class_id = obj_meta->class_id,
    .confidence = obj_meta->confidence,
    .frame_width = app_config.streammux.width,
    .frame_height = app_config.streammux.height,

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

static void
set_custom_bbox(NvDsObjectMeta *obj_meta)
{
  guint border_width = 6;
  guint font_size = 50;

  gfloat x_offset = obj_meta->rect_params.left - border_width * 0.5f;
  gfloat y_offset = obj_meta->rect_params.top - font_size * 2 + border_width * 0.5f + 1;

  // Set display text to show object ID
  g_snprintf(obj_meta->text_params.display_text, app_config.osd_text.max_display_len, "ID: %lu", obj_meta->object_id);

  obj_meta->rect_params.border_width = border_width;
  obj_meta->rect_params.border_color.red = 0.0;
  obj_meta->rect_params.border_color.green = 0.0;
  obj_meta->rect_params.border_color.blue = 1.0;
  obj_meta->rect_params.border_color.alpha = 1.0;
  obj_meta->text_params.font_params.font_name = (gchar *) "Ubuntu";
  obj_meta->text_params.font_params.font_size = font_size;
  obj_meta->text_params.x_offset = (guint) MIN(app_config.streammux.width - 1, MAX(0, x_offset));
  obj_meta->text_params.y_offset = (guint) MIN(app_config.streammux.height - 1, MAX(0, y_offset));
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

static GstPadProbeReturn
nvosd_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
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


    // Add NTP timestamp overlay (once per frame)
    if (frame_meta->ntp_timestamp) {
      NvDsDisplayMeta *display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
      
      // Convert NTP timestamp to human-readable format
      gdouble timestamp_sec = (gdouble)frame_meta->ntp_timestamp / 1e9;
      time_t timestamp_time = (time_t)timestamp_sec;
      struct tm *tm_info = localtime(&timestamp_time);
      
      gchar timestamp_str[128];
      strftime(timestamp_str, sizeof(timestamp_str), "%Y-%m-%d %H:%M:%S", tm_info);
      
      // Add milliseconds
      gint millisec = (gint)((timestamp_sec - (time_t)timestamp_sec) * 1000);
      gchar full_timestamp[128];
      g_snprintf(full_timestamp, sizeof(full_timestamp), "FRAME %d, NTP: %s.%03d", frame_meta->frame_num, timestamp_str, millisec);
      
      // Configure text parameters
      NvOSD_TextParams *txt_params = &display_meta->text_params[0];
      display_meta->num_labels = 1;
      
      txt_params->display_text = g_strdup(full_timestamp);
      txt_params->x_offset = app_config.osd_text.ntp_text_x_offset;
      txt_params->y_offset = app_config.osd_text.ntp_text_y_offset;
      
      txt_params->font_params.font_name = (gchar*)"Ubuntu";
      txt_params->font_params.font_size = app_config.osd_text.ntp_text_font_size;
      txt_params->font_params.font_color.red = 1.0;
      txt_params->font_params.font_color.green = 1.0;
      txt_params->font_params.font_color.blue = 1.0;
      txt_params->font_params.font_color.alpha = 1.0;
      
      txt_params->set_bg_clr = 1;
      txt_params->text_bg_clr.red = 0.0;
      txt_params->text_bg_clr.green = 0.0;
      txt_params->text_bg_clr.blue = 0.0;
      txt_params->text_bg_clr.alpha = 0.7;
      
      nvds_add_display_meta_to_frame(frame_meta, display_meta);
    }

    // Only draw if inference was done on this frame
    if (!frame_meta->bInferDone) {
      continue;
    }

    NvDsDisplayMeta *display_meta = NULL;
    NvDsMetaList *l_obj = NULL;
    for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) (l_obj->data);

      set_custom_bbox(obj_meta);

      // Draw landmarks (circles) if available
      if (obj_meta->mask_params.data && obj_meta->mask_params.size > 0) {
        guint num_joints = obj_meta->mask_params.size / (sizeof(float) * 3);
        
        gfloat gain = MIN((gfloat) obj_meta->mask_params.width / app_config.streammux.width, 
                          (gfloat) obj_meta->mask_params.height / app_config.streammux.height);
        gfloat pad_x = (obj_meta->mask_params.width - app_config.streammux.width * gain) * 0.5f;
        gfloat pad_y = (obj_meta->mask_params.height - app_config.streammux.height * gain) * 0.5f;

        for (guint i = 0; i < num_joints; ++i) {
          gfloat xc = (obj_meta->mask_params.data[i * 3 + 0] - pad_x) / gain;
          gfloat yc = (obj_meta->mask_params.data[i * 3 + 1] - pad_y) / gain;
          gfloat confidence = obj_meta->mask_params.data[i * 3 + 2];

          if (confidence < 0.5) {
            continue;
          }

          if (!display_meta || display_meta->num_circles == MAX_ELEMENTS_IN_DISPLAY_META) {
            display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
            nvds_add_display_meta_to_frame(frame_meta, display_meta);
          }

          NvOSD_CircleParams *circle_params = &display_meta->circle_params[display_meta->num_circles];
          circle_params->xc = (guint) MIN(app_config.streammux.width - 1, MAX(0, xc));
          circle_params->yc = (guint) MIN(app_config.streammux.height - 1, MAX(0, yc));
          circle_params->radius = 6;
          circle_params->circle_color.red = 1.0;
          circle_params->circle_color.green = 1.0;
          circle_params->circle_color.blue = 1.0;
          circle_params->circle_color.alpha = 1.0;
          circle_params->has_bg_color = 1;
          circle_params->bg_color.red = 0.0;
          circle_params->bg_color.green = 0.0;
          circle_params->bg_color.blue = 1.0;
          circle_params->bg_color.alpha = 1.0;
          display_meta->num_circles++;
        }
      }
    }

  }
  
  gst_buffer_unmap(buf, &map_info);

  return GST_PAD_PROBE_OK;
}


static gboolean
bus_call(GstBus *bus, GstMessage *message, gpointer user_data)
{
  GMainLoop *loop = (GMainLoop *) user_data;
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
    {
      GST_DEBUG("EOS");
      g_main_loop_quit(loop);
      break;
    }
    case GST_MESSAGE_WARNING:
    {
      gchar *debug;
      GError *error;
      gst_message_parse_warning(message, &error, &debug);
      GST_WARNING("%s - %s", error->message, debug);
      g_free(debug);
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_ERROR:
    {
      gchar *debug;
      GError *error;
      gst_message_parse_error(message, &error, &debug);
      GST_ERROR("%s - %s", error->message, debug);
      g_free(debug);
      g_error_free(error);
      g_main_loop_quit(loop);
      break;
    }
    default:
      break;
  }
  return TRUE;
}


// =============================================================================
// JSON File Save Helper
// =============================================================================

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
    surface = NULL;
  }
  
  // Process each frame in batch
  NvDsMetaList *l_frame = NULL;
  for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);

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
      if (frame_meta->obj_meta_list != NULL) {
        image_rel_path = save_frame_to_jpeg(surface, frame_meta,
                          app_config.frame_save.dir, app_config.frame_save.quality);
        GST_DEBUG("Saved frame %d to %s",
                  frame_meta->frame_num,
                  image_rel_path ? image_rel_path : "NULL");
      }
      //}
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
        obj_json = process_object(frame_meta, obj_meta, surface, image_rel_path);
      }

      if (obj_json)
        g_ptr_array_add(obj_jsons, obj_json);

      // Free mask_params after processing
      if (obj_meta->mask_params.data) {
        g_free(obj_meta->mask_params.data);
        obj_meta->mask_params.data = NULL;
        obj_meta->mask_params.width = 0;
        obj_meta->mask_params.height = 0;
        obj_meta->mask_params.size = 0;
      }
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
// Source Bin Creation
// =============================================================================
static void
uridecodebin_child_added_callback(GstChildProxy *child_proxy, GObject *object, gchar *name, gpointer user_data)
{
  if (g_strrstr(name, "decodebin")) {
    g_signal_connect(object, "child-added", G_CALLBACK(uridecodebin_child_added_callback), user_data);
  }
  else if (g_strrstr(name, "nvv4l2decoder")) {
    g_object_set(object, "drop-frame-interval", 0, "num-extra-surfaces", 1, "qos", 0, NULL);
    if (app_config.jetson) {
      g_object_set(object, "enable-max-performance", 1, NULL);
    }
    else {
      g_object_set(object, "cudadec-memtype", 0, "gpu-id", app_config.gpu_id, NULL);
    }
  }
}

static void
uridecodebin_pad_added_callback(GstElement *decodebin, GstPad *pad, gpointer user_data)
{
  GstPad *nvstreammux_sink_pad = (GstPad *) user_data;

  GstCaps *caps = gst_pad_get_current_caps(pad);
  if (!caps) {
    caps = gst_pad_query_caps(pad, NULL);
  }

  const GstStructure *str = gst_caps_get_structure(caps, 0);
  const gchar *name = gst_structure_get_name(str);
  GstCapsFeatures *features = gst_caps_get_features(caps, 0);

  if (!strncmp(name, "video", 5)) {
    if (gst_caps_features_contains(features, "memory:NVMM")) {
      if (gst_pad_link(pad, nvstreammux_sink_pad) != GST_PAD_LINK_OK) {
        GST_ERROR("Failed to link source to nvstreammux sink pad");
      }
    }
    else {
      GST_ERROR("decodebin did not pick NVIDIA decoder plugin");
    }
  }

  gst_caps_unref(caps);
}

static GstElement *
create_uridecodebin(guint stream_id, const gchar *uri, GstElement *nvstreammux)
{
  gchar bin_name[32] = { };
  g_snprintf(bin_name, 32, "source-bin-%04d", stream_id);

  GstElement *uridecodebin = gst_element_factory_make("uridecodebin", bin_name);

  if (g_strrstr(uri, "rtsp://")) {
    configure_source_for_ntp_sync(uridecodebin);
  }

  g_object_set(G_OBJECT(uridecodebin), "uri", uri, NULL);

  gchar pad_name[16];
  g_snprintf(pad_name, 16, "sink_%u", stream_id);

  GstPad *nvstreammux_sink_pad = gst_element_get_request_pad(nvstreammux, pad_name);
  if (!nvstreammux_sink_pad) {
    GST_ERROR("Failed to get nvstreammux %s pad", pad_name);
    return NULL;
  }

  g_signal_connect(G_OBJECT(uridecodebin), "pad-added", G_CALLBACK(uridecodebin_pad_added_callback),
      nvstreammux_sink_pad);
  g_signal_connect(G_OBJECT(uridecodebin), "child-added", G_CALLBACK(uridecodebin_child_added_callback), NULL);

  gst_object_unref(nvstreammux_sink_pad);

  return uridecodebin;
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
    detection_manager = detection_manager_new(FALSE, NULL, NULL,
                                             app_config.kafka.send_delay_sec,
                                             app_config.kafka.quality_improvement_threshold,
                                             app_config.kafka.cleanup_interval_sec,
                                             app_config.kafka.sent_record_ttl_sec,
                                             app_config.kafka.pending_ttl_sec);
    return;
  }
  
  detection_manager = detection_manager_new(TRUE, app_config.kafka.broker, app_config.kafka.topic,
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

gint
main(gint argc, char *argv[])
{
  // Initialize GStreamer and GST debug category before any GST_* logging
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(deepstream_debug_category, "deepstream", 0, "DeepStream Face App");

  // ============================================================================
  // Parse command-line options
  if (!parse_command_line(argc, argv)) {
    g_printerr("ERROR - Failed to parse command-line options\n");
    return -1;
  }
  
  //================================================
  // Debug: Print what was actually parsed
  GST_INFO("\n");
  GST_INFO("DEBUG - After parsing:\n");
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
  GST_INFO("\n");

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
      return -1;
    }
    
    GST_INFO("Frame saving enabled: dir=%s, quality=%u", 
            app_config.frame_save.dir, app_config.frame_save.quality);
  }
  // ============================================================================

  gint current_device = -1;
  cudaGetDevice(&current_device);
 
  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, current_device);

  if (prop.integrated) {
    app_config.jetson = TRUE;
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

  GMainLoop *loop = g_main_loop_new(NULL, FALSE);

  _intr_setup();
  g_timeout_add(400, check_for_interrupt, &loop);

  // Start periodic check for pending detections
  if (app_config.kafka.enabled && detection_manager) {
    g_timeout_add(500, detection_manager_process_pending_callback, NULL);
  }

  // ============================================================================
  // Create GStreamer pipeline
  GST_INFO("Creating GStreamer pipeline...");
  GstElement *pipeline = gst_pipeline_new("deepstream");
  if (!pipeline) {
    g_printerr("ERROR - Failed to create pipeline\n");
    return -1;
  }

  GstElement *nvstreammux = gst_element_factory_make("nvstreammux", "nvstreammux");
  if (!nvstreammux || !gst_bin_add(GST_BIN(pipeline), nvstreammux)) {
    g_printerr("ERROR - Failed to create nvstreammux\n");
    return -1;
  }

  // GstElement *uridecodebin = create_uridecodebin(0, SOURCE, nvstreammux);
  // if (!uridecodebin || !gst_bin_add(GST_BIN(pipeline), uridecodebin)) {
  //   g_printerr("ERROR - Failed to create uridecodebin\n");
  //   return -1;
  // }
  for (guint i = 0; i < app_config.source.count; i++) {
    GstElement *uridecodebin = create_uridecodebin(i, app_config.source.uris[i], nvstreammux);
    if (!uridecodebin || !gst_bin_add(GST_BIN(pipeline), uridecodebin)) {
      g_printerr("ERROR - Failed to create uridecodebin for source %d\n", i);
      return -1;
    }
  }

  GstElement *nvinfer = gst_element_factory_make(
      app_config.infer.use_triton ? "nvinferserver" : "nvinfer",
      app_config.infer.use_triton ? "nvinferserver" : "nvinfer");
  if (!nvinfer || !gst_bin_add(GST_BIN(pipeline), nvinfer)) {
    g_printerr("ERROR - Failed to create %s\n", app_config.infer.use_triton ? "nvinferserver" : "nvinfer");
    return -1;
  }

  // Secondary nvinferserver (Triton) — enabled only when infer2.config_file is set
  GstElement *nvinfer2 = NULL;
  if (app_config.infer2.config_file) {
    nvinfer2 = gst_element_factory_make("nvinferserver", "nvinferserver2");
    if (!nvinfer2 || !gst_bin_add(GST_BIN(pipeline), nvinfer2)) {
      g_printerr("ERROR - Failed to create nvinferserver2\n");
      return -1;
    }
  }

  GstElement *nvtracker = gst_element_factory_make("nvtracker", "nvtracker");
  if (!nvtracker || !gst_bin_add(GST_BIN(pipeline), nvtracker)) {
    g_printerr("ERROR - Failed to create nvtracker\n");
    return -1;
  }

  GstElement *nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvidconv");
  if (!nvvidconv || !gst_bin_add(GST_BIN(pipeline), nvvidconv)) {
    g_printerr("ERROR - Failed to create nvvideoconvert\n");
    return -1;
  }

  GstElement *capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
  if (!capsfilter || !gst_bin_add(GST_BIN(pipeline), capsfilter)) {
    g_printerr("ERROR - Failed to create capsfilter\n");
    return -1;
  }


  //================================================
  GstElement *tee = gst_element_factory_make("tee", "tee");
  if (!tee || !gst_bin_add(GST_BIN(pipeline), tee)) {
    g_printerr("ERROR - Failed to create tee\n");
    return -1;
  }


  //================================================
  // Tạo display sink có điều kiện
  GstElement *queue_display = NULL;
  GstElement *nvosd  = NULL;
  GstElement *nvsink = NULL;
  if (!app_config.display.disabled) {
    // queue
    queue_display = gst_element_factory_make("queue", "queue_display");
    if (!queue_display || !gst_bin_add(GST_BIN(pipeline), queue_display)) {
      g_printerr("ERROR - Failed to create queue_display\n");
      return -1;
    }
    
    g_object_set(G_OBJECT(queue_display),
        "max-size-buffers", app_config.queue.max_size_buffers,
        "leaky", app_config.queue.leaky,
        NULL);

    if (pipeline_monitor) {
      pipeline_monitor_add_queue(pipeline_monitor, "queue_display", queue_display);
    }

    // osd    
    nvosd = gst_element_factory_make("nvdsosd", "nvdsosd");
    if (!nvosd || !gst_bin_add(GST_BIN(pipeline), nvosd)) {
      g_printerr("ERROR - Failed to create nvdsosd\n");
      return -1;
    }
    
    g_object_set(G_OBJECT(nvosd), "process-mode", app_config.osd.process_mode, "qos", (gint)app_config.osd.qos, NULL);
    
    if (!app_config.jetson) {
      g_object_set(G_OBJECT(nvosd), "gpu_id", app_config.gpu_id, NULL);
    }



    // display sink
    if (app_config.jetson) {
      nvsink = gst_element_factory_make("nv3dsink", "nv3dsink");
      if (!nvsink || !gst_bin_add(GST_BIN(pipeline), nvsink)) {
        g_printerr("ERROR - Failed to create nv3dsink\n");
        return -1;
      }
    }
    else {
      nvsink = gst_element_factory_make("nveglglessink", "nveglglessink");
      if (!nvsink || !gst_bin_add(GST_BIN(pipeline), nvsink)) {
        g_printerr("ERROR - Failed to create nveglglessink\n");
        return -1;
      }
    }
    
    // Configure display sink
    g_object_set(G_OBJECT(nvsink), "async", (gint)app_config.display.async_sink, "sync", (gint)app_config.display.sync, "qos", (gint)app_config.display.qos, NULL);
    g_object_set(G_OBJECT(nvsink), "window-width", app_config.display.window_width, "window-height", app_config.display.window_height, NULL);
  }

  //================================================
  GstElement *queue_app = gst_element_factory_make("queue", "queue_app");
  if (!queue_app || !gst_bin_add(GST_BIN(pipeline), queue_app)) {
    g_printerr("ERROR - Failed to create queue_app\n");
    return -1;
  }

  g_object_set(G_OBJECT(queue_app),
    "max-size-buffers", app_config.queue.max_size_buffers,
    "leaky", app_config.queue.leaky,
    NULL);

  // Register queues with the pipeline monitor
  if (pipeline_monitor) {
    pipeline_monitor_add_queue(pipeline_monitor, "queue_app", queue_app);
  }

  GstElement *appsink = gst_element_factory_make("appsink", "appsink");
  if (!appsink || !gst_bin_add(GST_BIN(pipeline), appsink)) {
    g_printerr("ERROR - Failed to create appsink\n");
    return -1;
  }

  // Configure appsink
  g_object_set(G_OBJECT(appsink),
    "emit-signals", TRUE,
    "sync", (gint)app_config.appsink.sync,
    "max-buffers", app_config.appsink.max_buffers,
    "drop", (gint)app_config.appsink.drop,
    NULL);
  // Connect callback to appsink
  g_signal_connect(appsink, "new-sample", G_CALLBACK(appsink_new_sample_callback), NULL);

  
  GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=RGBA");
  g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
  gst_caps_unref(caps);

  g_object_set(G_OBJECT(nvstreammux),
     "batch-size", app_config.streammux.batch_size,
     "batched-push-timeout", app_config.streammux.batched_push_timeout,
     "width", app_config.streammux.width, "height", app_config.streammux.height, 
     "live-source", 1,
     NULL);
  g_object_set(G_OBJECT(nvinfer), "config-file-path", app_config.infer.config_file, "qos", (gint)app_config.infer.qos, NULL);
  if (nvinfer2) {
    g_object_set(G_OBJECT(nvinfer2), "config-file-path", app_config.infer2.config_file, "qos", (gint)app_config.infer2.qos, NULL);
  }
  g_object_set(G_OBJECT(nvtracker), "tracker-width", app_config.tracker.width, "tracker-height", app_config.tracker.height,
      "ll-lib-file", app_config.tracker.ll_lib_file,
      "ll-config-file", app_config.tracker.ll_config_file,
      "gpu-id", app_config.gpu_id, 
      "display-tracking-id", (gint)app_config.tracker.display_tracking_id, 
      NULL);

  // if (g_strrstr(SOURCE, "file://")) {
  //   g_object_set(G_OBJECT(nvstreammux), "live-source", 0, NULL);
  // }
  // Check if all sources are file-based (non-live)
  gboolean all_file_sources = TRUE;
  for (guint i = 0; i < app_config.source.count; i++) {
    if (!g_strrstr(app_config.source.uris[i], "file://")) {
      all_file_sources = FALSE;
      break;
    }
  }
  if (all_file_sources) {
    g_print("All sources are file-based. Setting live-source to 0.\n");
    g_object_set(G_OBJECT(nvstreammux), "live-source", 0, NULL);
  }

  if (!app_config.jetson) {
    g_object_set(G_OBJECT(nvstreammux), "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE, "gpu_id", app_config.gpu_id, NULL);
    if (!app_config.infer.use_triton) {
      g_object_set(G_OBJECT(nvinfer), "gpu_id", app_config.gpu_id, NULL);
    }
    g_object_set(G_OBJECT(nvvidconv), "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE, "gpu_id", app_config.gpu_id, NULL);
  }

  //==============================================
  // Link the elements together
  // Pipeline: nvstreammux -> nvinfer -> [nvinfer2 (Triton)] -> nvtracker -> nvvidconv -> capsfilter -> tee
  if (nvinfer2) {
    if (!gst_element_link_many(nvstreammux,nvinfer, nvinfer2, nvtracker,
                               nvvidconv, capsfilter, tee, NULL)) {
      g_printerr("ERROR - Failed to link pipeline elements (with nvinfer2) to tee\n");
      return -1;
    }
  } else {
    if (!gst_element_link_many(nvstreammux, nvinfer, nvtracker,
                               nvvidconv, capsfilter, tee, NULL)) {
      g_printerr("ERROR - Failed to link pipeline elements to tee\n");
      return -1;
    }
  }

  // Link: tee -> queue_app -> appsink
  if (!gst_element_link_many(tee, queue_app, appsink, NULL)) {
    g_printerr("ERROR - Failed to link tee to appsink\n");
    return -1;
  }

  // Link display branch (conditional)
 
  if (!app_config.display.disabled) {
     // Link: tee -> queue_display -> nvosd -> nvsink
    if (!gst_element_link_many(tee, queue_display, nvosd, nvsink, NULL)) {
      g_printerr("ERROR - Failed to link tee to display sink\n");
      return -1;
    }
  } else {
    // Tạo fakesink để terminate tee branch khi không có display
    GstElement *fakesink = gst_element_factory_make("fakesink", "fakesink");
    if (!fakesink || !gst_bin_add(GST_BIN(pipeline), fakesink)) {
      g_printerr("ERROR - Failed to create fakesink\n");
      return -1;
    }
    
    g_object_set(G_OBJECT(fakesink), "async", FALSE, "sync", FALSE, NULL);

    if (!gst_element_link_many(tee, fakesink, NULL)) {
      g_printerr("ERROR - Failed to link tee to fakesink\n");
      return -1;
    }
  }
  //==============================================

  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  guint bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
  gst_object_unref(bus);

  // ===============================================
  // perf measurement and osd sink pad probe
  NvDsAppPerfStructInt *perf_struct = (NvDsAppPerfStructInt *) g_malloc0(sizeof(NvDsAppPerfStructInt)); 
  
  GstPad *perf_pad = NULL;
  if(!app_config.display.disabled) {
    GstPad *nvosd_sink_pad = gst_element_get_static_pad(nvosd, "sink");
    perf_pad = nvosd_sink_pad;
    if (!nvosd_sink_pad) {
      g_printerr("ERROR - Failed to get nvosd sink pad\n");
      return -1;
    }

    gst_pad_add_probe(nvosd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                      nvosd_sink_pad_buffer_probe, NULL, NULL);
    gst_object_unref(nvosd_sink_pad);
  }else{
    perf_pad = gst_element_get_static_pad(tee, "sink");
    if (!perf_pad) {
      g_printerr("ERROR - Failed to get fakesink sink pad\n");
      return -1;
    }
  }
  
  enable_perf_measurement(perf_struct, perf_pad, app_config.source.count, app_config.perf_measurement_interval_sec, 0, perf_cb);

  // ===============================================
  // Start the pipeline
  GST_INFO("Starting GStreamer pipeline...\n");
  gst_element_set_state(pipeline, GST_STATE_PAUSED);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("ERROR - Failed to set pipeline to playing\n");
    return -1;
  }

  GST_DEBUG("\n");

  g_main_loop_run(loop);

  gst_element_set_state(pipeline, GST_STATE_NULL);

  // ===============================================
  // Print final pipeline metrics report
  if (pipeline_monitor) {
    g_print("\n=== FINAL PIPELINE METRICS REPORT ===\n");
    pipeline_monitor_print_report(pipeline_monitor);
    pipeline_monitor_free(pipeline_monitor);
    pipeline_monitor = NULL;
  }

  // Cleanup detection manager
  cleanup_detection_manager();

  g_free(perf_struct);

  config_free();
  gst_object_unref(GST_OBJECT(pipeline));
  g_source_remove(bus_watch_id);
  g_main_loop_unref(loop);

  return 0;
}
