#include "deepstream.h"
#include <jpeglib.h>
#include <setjmp.h>
#include "nvbufsurftransform.h"

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

static void
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
    return;
  }

  // Assess face quality
  gboolean is_good_face = FALSE;
  gdouble quality_score = 0.0;
  FaceQualityMetrics metrics = {0};

  if (!assess_face_quality(landmarks, num_landmarks, 
                           &is_good_face, &quality_score, &metrics)) {
    GST_WARNING("Failed to assess face quality for object_id=%lu", obj_meta->object_id);
    return;
  }

  GST_DEBUG("Face quality for object_id=%lu: is_good=%s, score=%.3f",
            obj_meta->object_id, is_good_face ? "true" : "false", quality_score);

  // Encode cropped face image if enabled
  CropBox crop_box;
  calculate_crop_box(obj_meta, &crop_box, STREAMMUX_WIDTH, STREAMMUX_HEIGHT);
  gchar *face_image_base64 = NULL;
  if (ENABLE_CROP_IMAGE && surface) {
    face_image_base64 = encode_crop_to_base64_jpeg(surface, &crop_box, 85, frame_meta->batch_id);
  }

  // Create face context
  FaceContext ctx = {
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
    //
    .frame_timestamp = frame_timestamp,
    .frame_num = frame_meta->frame_num,
    .source_id = frame_meta->source_id,
    .object_id = obj_meta->object_id,
    .class_id = obj_meta->class_id,
    .confidence = obj_meta->confidence,
  };

  // Process face detection and send to Kafka
  if (KAFKA_ENABLED && detection_manager && detection_manager_is_enabled(detection_manager)) {
      // Build JSON payload
      gchar *json_data = build_detection_json(&ctx);

      // Queue detection for Kafka
      detection_manager_queue(detection_manager,ctx.source_id,  ctx.object_id,
                              ctx.quality_score, json_data);         
      g_free(json_data);
  }

  // Cleanup
  g_free(face_image_base64);
  g_free(landmarks);
}

static void
set_custom_bbox(NvDsObjectMeta *obj_meta)
{
  guint border_width = 6;
  guint font_size = 50;

  gfloat x_offset = obj_meta->rect_params.left - border_width * 0.5f;
  gfloat y_offset = obj_meta->rect_params.top - font_size * 2 + border_width * 0.5f + 1;

  // Set display text to show object ID
  g_snprintf(obj_meta->text_params.display_text, MAX_DISPLAY_LEN, "ID: %lu", obj_meta->object_id);

  obj_meta->rect_params.border_width = border_width;
  obj_meta->rect_params.border_color.red = 0.0;
  obj_meta->rect_params.border_color.green = 0.0;
  obj_meta->rect_params.border_color.blue = 1.0;
  obj_meta->rect_params.border_color.alpha = 1.0;
  obj_meta->text_params.font_params.font_name = (gchar *) "Ubuntu";
  obj_meta->text_params.font_params.font_size = font_size;
  obj_meta->text_params.x_offset = (guint) MIN(STREAMMUX_WIDTH - 1, MAX(0, x_offset));
  obj_meta->text_params.y_offset = (guint) MIN(STREAMMUX_HEIGHT - 1, MAX(0, y_offset));
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
      
      gchar timestamp_str[MAX_DISPLAY_LEN];
      strftime(timestamp_str, sizeof(timestamp_str), "%Y-%m-%d %H:%M:%S", tm_info);
      
      // Add milliseconds
      gint millisec = (gint)((timestamp_sec - (time_t)timestamp_sec) * 1000);
      gchar full_timestamp[MAX_DISPLAY_LEN];
      g_snprintf(full_timestamp, sizeof(full_timestamp), "FRAME %d, NTP: %s.%03d", frame_meta->frame_num, timestamp_str, millisec);
      
      // Configure text parameters
      NvOSD_TextParams *txt_params = &display_meta->text_params[0];
      display_meta->num_labels = 1;
      
      txt_params->display_text = g_strdup(full_timestamp);
      txt_params->x_offset = NTP_TEXT_X_OFFSET;
      txt_params->y_offset = NTP_TEXT_Y_OFFSET;
      
      txt_params->font_params.font_name = "Ubuntu";
      txt_params->font_params.font_size = NTP_TEXT_FONT_SIZE;
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
        
        gfloat gain = MIN((gfloat) obj_meta->mask_params.width / STREAMMUX_WIDTH, 
                          (gfloat) obj_meta->mask_params.height / STREAMMUX_HEIGHT);
        gfloat pad_x = (obj_meta->mask_params.width - STREAMMUX_WIDTH * gain) * 0.5f;
        gfloat pad_y = (obj_meta->mask_params.height - STREAMMUX_HEIGHT * gain) * 0.5f;

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
          circle_params->xc = (guint) MIN(STREAMMUX_WIDTH - 1, MAX(0, xc));
          circle_params->yc = (guint) MIN(STREAMMUX_HEIGHT - 1, MAX(0, yc));
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

    // check frame is infer done
    if (frame_meta->bInferDone == FALSE) {
      GST_TRACE("Frame %d inference not done yet", frame_meta->frame_num);
      continue;
    }

    GST_DEBUG("Processing frame %d with %d objects",
              frame_meta->frame_num, frame_meta->num_obj_meta);

    gchar *image_rel_path = NULL;
    // Save frame to disk if enabled
    if (ENABLE_FRAME_SAVE && surface_valid && FRAME_SAVE_DIR) {
     
      //if (frame_meta->frame_num % FRAME_SAVE_INTERVAL == 0) {
      // Check if frame has any detected objects (faces)
      if (frame_meta->obj_meta_list != NULL) {
        image_rel_path = save_frame_to_jpeg(surface, frame_meta,
                          FRAME_SAVE_DIR, FRAME_SAVE_QUALITY);
        GST_DEBUG("Saved frame %d to %s",
                  frame_meta->frame_num,
                  image_rel_path ? image_rel_path : "NULL");
      }
      //}
    }
    
    // Process each object in frame
    NvDsMetaList *l_obj = NULL;
    for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *)(l_obj->data);
      
      // Process face with surface parameter
      process_object(frame_meta, obj_meta, surface, image_rel_path);

      // Free mask_params sau khi đã xử lý xong
      if (obj_meta->mask_params.data) {
        g_free(obj_meta->mask_params.data);
        obj_meta->mask_params.data = NULL;
        obj_meta->mask_params.width = 0;
        obj_meta->mask_params.height = 0;
        obj_meta->mask_params.size = 0;
      }
    }

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
    if (JETSON) {
      g_object_set(object, "enable-max-performance", 1, NULL);
    }
    else {
      g_object_set(object, "cudadec-memtype", 0, "gpu-id", GPU_ID, NULL);
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
  if (!KAFKA_ENABLED) {
    detection_manager = detection_manager_new(FALSE, NULL, NULL,
                                             KAFKA_SEND_DELAY_SEC,
                                             KAFKA_QUALITY_IMPROVEMENT_THRESHOLD,
                                             KAFKA_CLEANUP_INTERVAL_SEC,
                                             KAFKA_SENT_RECORD_TTL_SEC,
                                             KAFKA_PENDING_TTL_SEC);
    return;
  }
  
  detection_manager = detection_manager_new(TRUE, KAFKA_BROKER, KAFKA_TOPIC,
                                           KAFKA_SEND_DELAY_SEC,
                                           KAFKA_QUALITY_IMPROVEMENT_THRESHOLD,
                                           KAFKA_CLEANUP_INTERVAL_SEC,
                                           KAFKA_SENT_RECORD_TTL_SEC,
                                           KAFKA_PENDING_TTL_SEC);
  
  if (!detection_manager_init_kafka(detection_manager)) {
    GST_WARNING("WARNING - Failed to initialize Kafka, running without Kafka\n");
  }
  
  GST_DEBUG("Detection manager initialized (Kafka: %s, Topic: %s)\n",
          KAFKA_BROKER, KAFKA_TOPIC);
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
  GST_INFO("\n");
  // Debug: Print what was actually parsed
  GST_INFO("DEBUG - After parsing:\n");
  //GST_INFO("SOURCE: %s", SOURCE);
  GST_INFO("  NUM_SOURCES: %d", NUM_SOURCES);
  if (SOURCES) {
    for (guint i = 0; i < NUM_SOURCES; i++) {
      GST_INFO("  SOURCES[%d]: %s", i, SOURCES[i]);
    }
  } else {
    GST_INFO("  SOURCES: (null)");
  }
  GST_INFO("INFER_CONFIG: %s", INFER_CONFIG);
  GST_INFO("STREAMMUX_BATCH_SIZE: %d", STREAMMUX_BATCH_SIZE);
  GST_INFO("STREAMMUX_WIDTH: %d", STREAMMUX_WIDTH);
  GST_INFO("STREAMMUX_HEIGHT: %d", STREAMMUX_HEIGHT);
  GST_INFO("GPU_ID: %d", GPU_ID);
  GST_INFO("PERF_MEASUREMENT_INTERVAL_SEC: %d", PERF_MEASUREMENT_INTERVAL_SEC);
  GST_INFO("JETSON: %s", JETSON ? "TRUE" : "FALSE");
  GST_INFO("USE_TRITON: %s", USE_TRITON ? "TRUE" : "FALSE");
  if (KAFKA_ENABLED) {
    GST_INFO("KAFKA_BROKER: %s", KAFKA_BROKER);
    GST_INFO("KAFKA_TOPIC: %s", KAFKA_TOPIC);
    GST_INFO("KAFKA_SEND_DELAY_SEC: %.1f", KAFKA_SEND_DELAY_SEC);
    GST_INFO("KAFKA_QUALITY_IMPROVEMENT_THRESHOLD: %.2f", KAFKA_QUALITY_IMPROVEMENT_THRESHOLD);
  }
  GST_INFO("ENABLE_CROP_IMAGE: %s", ENABLE_CROP_IMAGE ? "TRUE" : "FALSE");
  if (ENABLE_FRAME_SAVE) {
    GST_INFO("FRAME_SAVE_DIR: %s", FRAME_SAVE_DIR);
    GST_INFO("FRAME_SAVE_QUALITY: %u", FRAME_SAVE_QUALITY);
  }
  GST_INFO("\n");

  // wait user to press enter key to start
  if (WAIT_FOR_USER_INPUT) {
    g_print("Press ENTER to start processing ...\n");
    getchar();
  }
 
  // ============================================================================
  // Initialize frame save directory if enabled
  if (ENABLE_FRAME_SAVE) {
    if (!FRAME_SAVE_DIR) {
      FRAME_SAVE_DIR = g_strdup("/app/outputs/frames");
    }
    
    if (!ensure_frame_save_directory(FRAME_SAVE_DIR)) {
      g_printerr("ERROR - Failed to create frame save directory: %s\n", FRAME_SAVE_DIR);
      return -1;
    }
    
    GST_INFO("Frame saving enabled: dir=%s, quality=%u", 
            FRAME_SAVE_DIR, FRAME_SAVE_QUALITY);
  }
  // ============================================================================

  gint current_device = -1;
  cudaGetDevice(&current_device);
 
  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, current_device);

  if (prop.integrated) {
    JETSON = TRUE;
  }

  // ============================================================================
  // Initialize detection manager
  GST_INFO("Initializing detection manager...");
  init_detection_manager();

  GMainLoop *loop = g_main_loop_new(NULL, FALSE);

  _intr_setup();
  g_timeout_add(400, check_for_interrupt, &loop);

  // Start periodic check for pending detections
  if (KAFKA_ENABLED && detection_manager) {
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
  for (guint i = 0; i < NUM_SOURCES; i++) {
    GstElement *uridecodebin = create_uridecodebin(i, SOURCES[i], nvstreammux);
    if (!uridecodebin || !gst_bin_add(GST_BIN(pipeline), uridecodebin)) {
      g_printerr("ERROR - Failed to create uridecodebin for source %d\n", i);
      return -1;
    }
  }

  GstElement *nvinfer = gst_element_factory_make(
      USE_TRITON ? "nvinferserver" : "nvinfer",
      USE_TRITON ? "nvinferserver" : "nvinfer");
  if (!nvinfer || !gst_bin_add(GST_BIN(pipeline), nvinfer)) {
    g_printerr("ERROR - Failed to create %s\n", USE_TRITON ? "nvinferserver" : "nvinfer");
    return -1;
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

  // GstElement *capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
  // if (!capsfilter || !gst_bin_add(GST_BIN(pipeline), capsfilter)) {
  //   g_printerr("ERROR - Failed to create capsfilter\n");
  //   return -1;
  // }


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
  if (!DISABLE_DISPLAY) {
    // queue
    queue_display = gst_element_factory_make("queue", "queue_display");
    if (!queue_display || !gst_bin_add(GST_BIN(pipeline), queue_display)) {
      g_printerr("ERROR - Failed to create queue_display\n");
      return -1;
    }
    
    g_object_set(G_OBJECT(queue_display),
        "max-size-buffers", 5,
        "leaky", 2,
        NULL);

    // osd    
    nvosd = gst_element_factory_make("nvdsosd", "nvdsosd");
    if (!nvosd || !gst_bin_add(GST_BIN(pipeline), nvosd)) {
      g_printerr("ERROR - Failed to create nvdsosd\n");
      return -1;
    }
    
    g_object_set(G_OBJECT(nvosd), "process-mode", MODE_GPU, "qos", 0, NULL);
    
    if (!JETSON) {
      g_object_set(G_OBJECT(nvosd), "gpu_id", GPU_ID, NULL);
    }



    // display sink
    if (JETSON) {
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
    g_object_set(G_OBJECT(nvsink), "async", 0, "sync", 0, "qos", 0, NULL);
    g_object_set(G_OBJECT(nvsink), "window-width", 400, "window-height", 400, NULL);
  }

  //================================================
  GstElement *queue_app = gst_element_factory_make("queue", "queue_app");
  if (!queue_app || !gst_bin_add(GST_BIN(pipeline), queue_app)) {
    g_printerr("ERROR - Failed to create queue_app\n");
    return -1;
  }

  g_object_set(G_OBJECT(queue_app),
    "max-size-buffers", 5,
    "leaky", 2,
    NULL);

  GstElement *appsink = gst_element_factory_make("appsink", "appsink");
  if (!appsink || !gst_bin_add(GST_BIN(pipeline), appsink)) {
    g_printerr("ERROR - Failed to create appsink\n");
    return -1;
  }

  // Configure appsink
  g_object_set(G_OBJECT(appsink),
    "emit-signals", TRUE,      // appsink sẽ phát ra một tín hiệu mỗi khi có buffer mới đến. Bạn có thể kết nối hàm xử lý của mình với tín hiệu này bằng g_signal_connect.
    "sync", FALSE,             // Don't sync to clock
    "max-buffers", 5,          // Keep only 5 buffers to avoid memory buildup
    "drop", TRUE,              // Drop old buffers if queue is full
    NULL);
  // Connect callback to appsink
  g_signal_connect(appsink, "new-sample", G_CALLBACK(appsink_new_sample_callback), NULL);

  
  // GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=RGBA");
  // g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
  // gst_caps_unref(caps);

  g_object_set(G_OBJECT(nvstreammux),
     "batch-size", STREAMMUX_BATCH_SIZE,
     "batched-push-timeout", 25000, // in microseconds
     "width", STREAMMUX_WIDTH, "height", STREAMMUX_HEIGHT, "live-source", 1, NULL);
  g_object_set(G_OBJECT(nvinfer), "config-file-path", INFER_CONFIG, "qos", 0, NULL);
  g_object_set(G_OBJECT(nvtracker), "tracker-width", 640, "tracker-height", 384,
      "ll-lib-file", "/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so",
      "ll-config-file", "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml",
      "gpu-id", GPU_ID, "display-tracking-id", 1, NULL);

  // if (g_strrstr(SOURCE, "file://")) {
  //   g_object_set(G_OBJECT(nvstreammux), "live-source", 0, NULL);
  // }
  // Check if all sources are file-based (non-live)
  gboolean all_file_sources = TRUE;
  for (guint i = 0; i < NUM_SOURCES; i++) {
    if (!g_strrstr(SOURCES[i], "file://")) {
      all_file_sources = FALSE;
      break;
    }
  }
  if (all_file_sources) {
    g_print("All sources are file-based. Setting live-source to 0.\n");
    g_object_set(G_OBJECT(nvstreammux), "live-source", 0, NULL);
  }

  if (!JETSON) {
    g_object_set(G_OBJECT(nvstreammux), "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE, "gpu_id", GPU_ID, NULL);
    if (!USE_TRITON) {
      g_object_set(G_OBJECT(nvinfer), "gpu_id", GPU_ID, NULL);
    }
    g_object_set(G_OBJECT(nvvidconv), "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE, "gpu_id", GPU_ID, NULL);
  }

  //==============================================
  // Link the elements together
  if (!gst_element_link_many(nvstreammux, nvinfer, nvtracker, 
                            //nvvidconv, 
                            //capsfilter,
                            tee, NULL)) {
    g_printerr("ERROR - Failed to link pipeline elements to tee\n");
    return -1;
  }

  // Link: tee -> queue_app -> appsink
  if (!gst_element_link_many(tee, queue_app, appsink, NULL)) {
    g_printerr("ERROR - Failed to link tee to appsink\n");
    return -1;
  }

  // Link display branch (conditional)
 
  if (!DISABLE_DISPLAY) {
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
  if(!DISABLE_DISPLAY) {
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
  
  enable_perf_measurement(perf_struct, perf_pad, NUM_SOURCES, PERF_MEASUREMENT_INTERVAL_SEC, 0, perf_cb);

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
  // Cleanup detection manager
  cleanup_detection_manager();

  g_free(perf_struct);

  config_free();
  gst_object_unref(GST_OBJECT(pipeline));
  g_source_remove(bus_watch_id);
  g_main_loop_unref(loop);

  return 0;
}
