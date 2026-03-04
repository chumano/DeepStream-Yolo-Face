#include "deepstream.h"
#include <jpeglib.h>
#include <sys/stat.h>

//  modules
#include "modules/config.h"
#include "modules/face.h"
#include "modules/face_analysis.h"
#include "modules/image_processing.h"
#include "modules/json_builder.h"
#include "modules/osd_probe.h"
#include "modules/pipeline_builder.h"
// GST_DEBUG_CATEGORY_STATIC to GST_DEBUG_CATEGORY  
//which makes the symbol externally visible so that osd_probe.c
GST_DEBUG_CATEGORY(deepstream_debug_category);
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
      // This variable holds the count for the current frame
      if (  frame_meta->obj_meta_list != NULL 
        && frame_meta->num_obj_meta > 0 ) {
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
  AppPipeline *ap = create_app_pipeline(
      loop,
      G_CALLBACK(appsink_new_sample_callback),
      pipeline_monitor);
  if (!ap) {
    g_printerr("ERROR - Failed to create pipeline\n");
    return -1;
  }

  // ===============================================
  // Start the pipeline
  GST_INFO("Starting GStreamer pipeline...\n");
  gst_element_set_state(ap->pipeline, GST_STATE_PAUSED);

  if (gst_element_set_state(ap->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("ERROR - Failed to set pipeline to playing\n");
    return -1;
  }

  GST_DEBUG("\n");

  g_main_loop_run(loop);
  g_print("\nPipeline stopped, performing cleanup...\n");

  gst_element_set_state(ap->pipeline, GST_STATE_NULL);

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

  config_free();
  destroy_app_pipeline(ap);
  g_main_loop_unref(loop);

  return 0;
}
