#include "config.h"

// =============================================================================
// Global AppConfig instance with compile-time defaults
// =============================================================================

AppConfig app_config = {
  .config_file = NULL,

  /* General */
  .gpu_id                        = 0,
  .jetson                        = FALSE,
  .perf_measurement_interval_sec = 5,
  .wait_for_user_input           = TRUE,
  .enable_crop_image             = TRUE,

  /* Sources */
  .source = {
    .uris  = NULL,
    .count = 0,
  },

  /* nvstreammux */
  .streammux = {
    .batch_size            = 1,
    .width                 = 1920,
    .height                = 1080,
    .enable_padding        = FALSE,
    .batched_push_timeout  = 25000,  /* µs */
  },

  /* nvinfer / nvinferserver */
  .infer = {
    .config_file = NULL,
    .use_triton  = FALSE,
    .qos         = FALSE,
  },

  /* secondary nvinferserver (Triton); disabled when config_file == NULL */
  .infer2 = {
    .config_file = NULL,
    .use_triton  = TRUE,   /* always Triton */
    .qos         = FALSE,
  },

  /* nvtracker */
  .tracker = {
    .width               = 640,
    .height              = 384,
    .ll_lib_file         = NULL,  /* set at runtime after detecting Jetson */
    .ll_config_file      = NULL,
    .display_tracking_id = TRUE,
  },

  /* nvdsosd */
  .osd = {
    .process_mode = 1,   /* MODE_GPU */
    .qos          = FALSE,
    .draw_landmarks = TRUE,
    .draw_custom_bbox = TRUE,
  },

  /* Display sink */
  .display = {
    .disabled      = FALSE,
    .window_width  = 400,
    .window_height = 400,
    .sync          = FALSE,
    .async_sink    = FALSE,
    .qos           = FALSE,
  },

  /* Shared queue settings */
  .queue = {
    .max_size_buffers = 5,
    .leaky            = 2,  /* downstream */
  },

  /* appsink */
  .appsink = {
    .max_buffers = 5,
    .drop        = TRUE,
    .sync        = FALSE,
  },

  /* Kafka */
  .kafka = {
    .enabled                      = FALSE,
    .broker                       = NULL,
    .topic                        = NULL,
    .event_topic                  = NULL,
    .send_delay_sec               = 2.0,
    .quality_improvement_threshold = 0.005,
    .sent_record_ttl_sec          = 60.0,
    .pending_ttl_sec              = 10.0,
    .cleanup_interval_sec         = 30.0,
  },

  /* Face quality */
  .face_quality = {
    .min_landmark_confidence = 0.5,
    .min_visible_landmarks   = 3,
    .face_quality_threshold  = 0.6,
    .min_frontal_score       = 0.7,
  },

  /* OSD text overlay */
  .osd_text = {
    .max_display_len   = 128,
    .ntp_text_x_offset = 30,
    .ntp_text_y_offset = 30,
    .ntp_text_font_size = 20,
  },

  /* Frame save */
  .frame_save = {
    .enabled                = TRUE,
    .dir                    = NULL,
    .quality                = 70,
    .exclude_letterbox      = TRUE,
    .save_all_frames        = FALSE,
    .pre_buffer_duration_sec = 5.0,
  },

  /* JSON save */
  .json_save = {
    .enabled = FALSE,
    .dir     = NULL,
  },

  /* Smart Record (nvurisrcbin, RTSP only) */
  .smart_record = {
    .enabled              = FALSE,
    .dir                  = NULL,
    .file_prefix          = NULL,  /* NULL → nvurisrcbin default "Smart_Record" */
    .cache_size_sec       = 30,
    .default_duration_sec = 20,
    .container            = 0,     /* 0 = MP4 */
  },

  /* File cleanup */
  .file_cleanup = {
    .enabled      = FALSE,
    .interval_sec = 300,   /* 5 minutes */
    .max_age_sec  = 600,  /* 10 minutes */
  },
};


// =============================================================================
// INI parsing helpers — reads multiple named sections
// =============================================================================

gboolean
parse_config_file(const gchar *config_file, GError **error)
{
  GKeyFile *kf = g_key_file_new();

  if (!g_key_file_load_from_file(kf, config_file, G_KEY_FILE_NONE, error)) {
    g_key_file_free(kf);
    return FALSE;
  }

  GError *ke = NULL;   /* per-key error — cleared after each lookup */

/* Convenience macros — section-aware */
#define GET_STR(sec, key, var) \
  do { \
    gchar *_v = g_key_file_get_string(kf, sec, key, &ke); \
    if (_v && !ke) { g_free(var); var = _v; } \
    else { g_clear_error(&ke); } \
  } while (0)

#define GET_STR_ARRAY(sec, key, var, cnt) \
  do { \
    gsize _len = 0; \
    gchar **_v = g_key_file_get_string_list(kf, sec, key, &_len, &ke); \
    if (_v && !ke) { g_strfreev(var); var = _v; cnt = (guint)_len; } \
    else { g_clear_error(&ke); } \
  } while (0)

#define GET_INT(sec, key, var) \
  do { \
    gint _v = g_key_file_get_integer(kf, sec, key, &ke); \
    if (!ke) { var = (typeof(var))_v; } else { g_clear_error(&ke); } \
  } while (0)

#define GET_DBL(sec, key, var) \
  do { \
    gdouble _v = g_key_file_get_double(kf, sec, key, &ke); \
    if (!ke) { var = _v; } else { g_clear_error(&ke); } \
  } while (0)

#define GET_BOOL(sec, key, var) \
  do { \
    gboolean _v = g_key_file_get_boolean(kf, sec, key, &ke); \
    if (!ke) { var = _v; } else { g_clear_error(&ke); } \
  } while (0)

  /* ── [app] ──────────────────────────────────────────────── */
  GET_INT ("app", "gpu-id",                   app_config.gpu_id);
  GET_INT ("app", "perf-measurement-interval",app_config.perf_measurement_interval_sec);
  GET_BOOL("app", "wait-for-user-input",      app_config.wait_for_user_input);
  GET_BOOL("app", "enable-crop-image",        app_config.enable_crop_image);

  /* ── [sources] ──────────────────────────────────────────── */
  GET_STR_ARRAY("sources", "uris", app_config.source.uris, app_config.source.count);

  /* ── [streammux] ─────────────────────────────────────────── */
  GET_INT("streammux", "batch-size",           app_config.streammux.batch_size);
  GET_INT("streammux", "width",                app_config.streammux.width);
  GET_INT("streammux", "height",               app_config.streammux.height);
  GET_BOOL("streammux", "enable-padding",      app_config.streammux.enable_padding);
  GET_INT("streammux", "batched-push-timeout", app_config.streammux.batched_push_timeout);

  /* ── [infer] ─────────────────────────────────────────────── */
  GET_STR ("infer", "config-file", app_config.infer.config_file);
  GET_BOOL("infer", "use-triton",  app_config.infer.use_triton);
  GET_BOOL("infer", "qos",         app_config.infer.qos);

  /* ── [infer2] — secondary Triton inference ───────────────── */
  GET_STR ("infer2", "config-file", app_config.infer2.config_file);
  GET_BOOL("infer2", "qos",         app_config.infer2.qos);

  /* ── [tracker] ───────────────────────────────────────────── */
  GET_INT ("tracker", "width",                app_config.tracker.width);
  GET_INT ("tracker", "height",               app_config.tracker.height);
  GET_STR ("tracker", "ll-lib-file",          app_config.tracker.ll_lib_file);
  GET_STR ("tracker", "ll-config-file",       app_config.tracker.ll_config_file);
  GET_BOOL("tracker", "display-tracking-id",  app_config.tracker.display_tracking_id);

  /* ── [osd] ───────────────────────────────────────────────── */
  GET_INT ("osd", "process-mode", app_config.osd.process_mode);
  GET_BOOL("osd", "qos",          app_config.osd.qos);
  GET_BOOL("osd", "draw-landmarks",  app_config.osd.draw_landmarks);
  GET_BOOL("osd", "draw-custom-bbox",app_config.osd.draw_custom_bbox);

  /* ── [display] ───────────────────────────────────────────── */
  GET_BOOL("display", "disabled",      app_config.display.disabled);
  GET_INT ("display", "window-width",  app_config.display.window_width);
  GET_INT ("display", "window-height", app_config.display.window_height);
  GET_BOOL("display", "sync",          app_config.display.sync);
  GET_BOOL("display", "async",         app_config.display.async_sink);
  GET_BOOL("display", "qos",           app_config.display.qos);

  /* ── [queue] ─────────────────────────────────────────────── */
  GET_INT("queue", "max-size-buffers", app_config.queue.max_size_buffers);
  GET_INT("queue", "leaky",            app_config.queue.leaky);

  /* ── [appsink] ───────────────────────────────────────────── */
  GET_INT ("appsink", "max-buffers", app_config.appsink.max_buffers);
  GET_BOOL("appsink", "drop",        app_config.appsink.drop);
  GET_BOOL("appsink", "sync",        app_config.appsink.sync);

  /* ── [kafka] ─────────────────────────────────────────────── */
  GET_STR ("kafka", "broker",             app_config.kafka.broker);
  GET_STR ("kafka", "topic",              app_config.kafka.topic);  
  GET_STR ("kafka", "event-topic",        app_config.kafka.event_topic);  
  GET_DBL ("kafka", "delay",              app_config.kafka.send_delay_sec);
  GET_DBL ("kafka", "quality-threshold",  app_config.kafka.quality_improvement_threshold);
  GET_DBL ("kafka", "sent-record-ttl",    app_config.kafka.sent_record_ttl_sec);
  GET_DBL ("kafka", "pending-ttl",        app_config.kafka.pending_ttl_sec);
  GET_DBL ("kafka", "cleanup-interval",   app_config.kafka.cleanup_interval_sec);

  /* ── [face_quality] ──────────────────────────────────────── */
  GET_DBL ("face_quality", "min-landmark-confidence", app_config.face_quality.min_landmark_confidence);
  GET_INT ("face_quality", "min-visible-landmarks",   app_config.face_quality.min_visible_landmarks);
  GET_DBL ("face_quality", "face-quality-threshold",  app_config.face_quality.face_quality_threshold);
  GET_DBL ("face_quality", "min-frontal-score",       app_config.face_quality.min_frontal_score);

  /* ── [osd_text] ──────────────────────────────────────────── */
  GET_INT("osd_text", "max-display-len",    app_config.osd_text.max_display_len);
  GET_INT("osd_text", "ntp-text-x-offset",  app_config.osd_text.ntp_text_x_offset);
  GET_INT("osd_text", "ntp-text-y-offset",  app_config.osd_text.ntp_text_y_offset);
  GET_INT("osd_text", "ntp-text-font-size", app_config.osd_text.ntp_text_font_size);

  /* ── [frame_save] ────────────────────────────────────────── */
  GET_BOOL("frame_save", "enabled",            app_config.frame_save.enabled);
  GET_STR ("frame_save", "dir",                app_config.frame_save.dir);
  GET_INT ("frame_save", "quality",            app_config.frame_save.quality);
  GET_BOOL("frame_save", "exclude-letterbox",  app_config.frame_save.exclude_letterbox);
  GET_BOOL("frame_save", "save-all-frames",    app_config.frame_save.save_all_frames);
  GET_DBL ("frame_save", "pre-buffer-duration",app_config.frame_save.pre_buffer_duration_sec);

  /* ── [json_save] ─────────────────────────────────────────── */
  GET_BOOL("json_save", "enabled", app_config.json_save.enabled);
  GET_STR ("json_save", "dir",     app_config.json_save.dir);

  /* ── [smart_record] ─────────────────────────────────────── */
  GET_BOOL("smart_record", "enabled",          app_config.smart_record.enabled);
  GET_STR ("smart_record", "dir",              app_config.smart_record.dir);
  GET_STR ("smart_record", "file-prefix",      app_config.smart_record.file_prefix);
  GET_INT ("smart_record", "cache-size",       app_config.smart_record.cache_size_sec);
  GET_INT ("smart_record", "default-duration", app_config.smart_record.default_duration_sec);
  GET_INT ("smart_record", "container",        app_config.smart_record.container);

  /* ── [file_cleanup] ──────────────────────────────────────── */
  GET_BOOL("file_cleanup", "enabled",      app_config.file_cleanup.enabled);
  GET_INT ("file_cleanup", "interval",     app_config.file_cleanup.interval_sec);
  GET_INT ("file_cleanup", "max-age",      app_config.file_cleanup.max_age_sec);

#undef GET_STR
#undef GET_STR_ARRAY
#undef GET_INT
#undef GET_DBL
#undef GET_BOOL

  g_key_file_free(kf);
  return TRUE;
}

// =============================================================================
// Command-line option entries (map to struct fields directly)
// =============================================================================

/* Temporary holders for options that need post-processing */
static gchar    *_opt_config_file   = NULL;
static gchar   **_opt_sources       = NULL;
static gchar    *_opt_infer_config  = NULL;
static gchar    *_opt_infer2_config = NULL;
static gchar    *_opt_kafka_broker  = NULL;
static gchar    *_opt_kafka_topic  = NULL;
static gchar    *_opt_frame_save_dir = NULL;

static GOptionEntry entries[] = {
  /* General */
  {"config",        'f', 0, G_OPTION_ARG_STRING,       &_opt_config_file,
   "INI configuration file",                             NULL},
  {"source",        's', 0, G_OPTION_ARG_STRING_ARRAY,  &_opt_sources,
   "Source URI (repeatable: -s uri1 -s uri2)",           "URI"},
  {"gpu-id",        'g', 0, G_OPTION_ARG_INT,           &app_config.gpu_id,
   "GPU id (default 0)",                                 NULL},

  /* Streammux */
  {"streammux-batch-size", 'b', 0, G_OPTION_ARG_INT, &app_config.streammux.batch_size,
   "Streammux batch-size (default 1)",                   NULL},
  {"streammux-width",      'w', 0, G_OPTION_ARG_INT, &app_config.streammux.width,
   "Streammux width (default 1920)",                     NULL},
  {"streammux-height",     'e', 0, G_OPTION_ARG_INT, &app_config.streammux.height,
   "Streammux height (default 1080)",                    NULL},

  /* Inference */
  {"infer-config",   'c', 0, G_OPTION_ARG_STRING, &_opt_infer_config,
   "nvinfer config file path",                              NULL},
  {"use-triton",     0,   0, G_OPTION_ARG_NONE,   &app_config.infer.use_triton,
   "Use nvinferserver (Triton) instead of nvinfer",         NULL},
  {"infer2-config",  0,   0, G_OPTION_ARG_STRING, &_opt_infer2_config,
   "Secondary nvinferserver (Triton) config file path",    NULL},

  /* Kafka */
  {"kafka-broker",           'k', 0, G_OPTION_ARG_STRING, &_opt_kafka_broker,
   "Kafka broker  (e.g. localhost:9092)",                NULL},
  {"kafka-topic",            't', 0, G_OPTION_ARG_STRING, &_opt_kafka_topic,
   "Kafka topic (default: face-detections)",             NULL},
  {"kafka-delay",            'd', 0, G_OPTION_ARG_DOUBLE, &app_config.kafka.send_delay_sec,
   "Delay before sending to Kafka in seconds",           NULL},
  {"kafka-quality-threshold",'q', 0, G_OPTION_ARG_DOUBLE, &app_config.kafka.quality_improvement_threshold,
   "Min quality delta to resend a detection",            NULL},

  /* Display */
  {"disable-display",   0, 0,                    G_OPTION_ARG_NONE, &app_config.display.disabled,
   "Disable video display output",                       NULL},

  /* Crop */
  {"disable-crop-image", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &app_config.enable_crop_image,
   "Disable face crop image in Kafka JSON",              NULL},

  /* Frame save */
  {"enable-frame-save",     0, 0, G_OPTION_ARG_NONE,   &app_config.frame_save.enabled,
   "Enable saving frames to disk",                       NULL},
  {"frame-save-dir",        0, 0, G_OPTION_ARG_STRING, &_opt_frame_save_dir,
   "Directory to save frames",                           NULL},
  {"frame-save-quality",    0, 0, G_OPTION_ARG_INT,    &app_config.frame_save.quality,
   "JPEG quality 0-100 (default 70)",                    NULL},
  {"save-all-frames",       0, 0, G_OPTION_ARG_NONE,   &app_config.frame_save.save_all_frames,
   "Save every frame (not just when face detected)",     NULL},
  {"pre-buffer-duration",   0, 0, G_OPTION_ARG_DOUBLE, &app_config.frame_save.pre_buffer_duration_sec,
   "Pre-detection buffer duration in seconds (default 1.0)", NULL},

  {NULL}
};


// =============================================================================
// Forward declaration for static function
// =============================================================================
static void print_app_config(void);
// =============================================================================
// parse_command_line
// =============================================================================

gint
parse_command_line(gint argc, char *argv[])
{
  /* ── Pass 1: look for -f / --config so we can load the INI first ── */
  for (gint i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "--config") == 0 || g_strcmp0(argv[i], "-f") == 0) {
      if (i + 1 < argc) {
        _opt_config_file = g_strdup(argv[i + 1]);
        g_print("Found config file: %s\n", _opt_config_file);
        break;
      }
    }
  }

  /* ── Load INI file (sets defaults for everything) ── */
  if (_opt_config_file) {
    g_print("Loading configuration from file...\n");
    GError *cfg_err = NULL;
    if (!parse_config_file(_opt_config_file, &cfg_err)) {
      g_printerr("ERROR - Failed to load config file: %s\n",
                 cfg_err ? cfg_err->message : "unknown error");
      if (cfg_err) g_error_free(cfg_err);
      return -1;
    }
    app_config.config_file = g_strdup(_opt_config_file);
    g_print("Configuration loaded successfully\n");
  }

  /* ── Parse remaining CLI flags (override INI values) ── */
  g_print("Parsing command line options...\n");
  GOptionContext *ctx = g_option_context_new("- DeepStream Face App");
  GOptionGroup   *grp = g_option_group_new("deepstream", NULL, NULL, NULL, NULL);
  GError         *err = NULL;

  g_option_group_add_entries(grp, entries);
  g_option_context_set_main_group(ctx, grp);
  g_option_context_add_group(ctx, gst_init_get_option_group());

  if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
    g_printerr("ERROR - %s\n", err->message);
    g_printerr("Run with --help to see available options\n");
    g_error_free(err);
    g_option_context_free(ctx);
    return -1;
  }
  g_option_context_free(ctx);

  /* ── Flush temp holders into struct ── */
  if (_opt_infer_config) {
    g_free(app_config.infer.config_file);
    app_config.infer.config_file = _opt_infer_config;
    _opt_infer_config = NULL;
  }
  if (_opt_infer2_config) {
    g_free(app_config.infer2.config_file);
    app_config.infer2.config_file = _opt_infer2_config;
    _opt_infer2_config = NULL;
  }
  if (_opt_sources) {
    g_strfreev(app_config.source.uris);
    app_config.source.uris  = _opt_sources;
    app_config.source.count = g_strv_length(_opt_sources);
    _opt_sources = NULL;
  }
  if (_opt_kafka_broker) {
    g_free(app_config.kafka.broker);
    app_config.kafka.broker = _opt_kafka_broker;
    _opt_kafka_broker = NULL;
  }
  if (_opt_kafka_topic) {
    g_free(app_config.kafka.topic);
    app_config.kafka.topic = _opt_kafka_topic;
    _opt_kafka_topic = NULL;
  }
  if (_opt_frame_save_dir) {
    g_free(app_config.frame_save.dir);
    app_config.frame_save.dir = _opt_frame_save_dir;
    _opt_frame_save_dir = NULL;
  }

  /* ── Validation ── */
  if (app_config.source.count == 0) {
    g_printerr("ERROR - No sources provided. Use -s <uri> to specify source(s)\n");
    return -1;
  }

  if (app_config.streammux.batch_size < app_config.source.count) {
    app_config.streammux.batch_size = app_config.source.count;
    g_print("Setting batch-size to %d to match number of sources\n",
            app_config.streammux.batch_size);
  }

  if (!app_config.infer.config_file) {
    g_printerr("ERROR - Infer config file not provided. Use -c <path>\n");
    return -1;
  }

  /* Derive kafka.enabled from broker presence */
  if (app_config.kafka.broker) {
    app_config.kafka.enabled = TRUE;
    if (!app_config.kafka.topic)
      app_config.kafka.topic = g_strdup("face-detections");
  }

  /* Default tracker lib/config paths if not set via INI/CLI */
  if (!app_config.tracker.ll_lib_file)
    app_config.tracker.ll_lib_file = g_strdup(
      "/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so");
  if (!app_config.tracker.ll_config_file)
    app_config.tracker.ll_config_file = g_strdup(
      "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml");

  g_print("Command line options parsed successfully\n");


  /* ── print final config ── */
  print_app_config();
  return 1;
}
// =============================================================================
// print_app_config — print all fields of AppConfig for debugging
// =============================================================================

static void print_app_config(void) {
  g_print("\n==== Final AppConfig ===="\
   "\n  config_file: %s"\
   "\n  gpu_id: %u"\
   "\n  jetson: %s"\
   "\n  perf_measurement_interval_sec: %u"\
   "\n  wait_for_user_input: %s"\
   "\n  enable_crop_image: %s"\
   "\n  sources.count: %u"\
   "\n  sources.uris: ",
   app_config.config_file ? app_config.config_file : "(none)",
   app_config.gpu_id,
   app_config.jetson ? "TRUE" : "FALSE",
   app_config.perf_measurement_interval_sec,
   app_config.wait_for_user_input ? "TRUE" : "FALSE",
   app_config.enable_crop_image ? "TRUE" : "FALSE",
   app_config.source.count);
  if (app_config.source.uris) {
    for (guint i = 0; i < app_config.source.count; ++i)
      g_print("%s%s", i == 0 ? "" : ", ", app_config.source.uris[i]);
  } else {
    g_print("(none)");
  }
  g_print("\n  streammux: batch_size=%u width=%u height=%u batched_push_timeout=%u enable_padding=%s",
    app_config.streammux.batch_size,
    app_config.streammux.width,
    app_config.streammux.height,
    app_config.streammux.batched_push_timeout,
    app_config.streammux.enable_padding ? "TRUE" : "FALSE");
  g_print("\n  infer: config_file=%s use_triton=%s qos=%s",
    app_config.infer.config_file ? app_config.infer.config_file : "(none)",
    app_config.infer.use_triton ? "TRUE" : "FALSE",
    app_config.infer.qos ? "TRUE" : "FALSE");
  g_print("\n  infer2 (triton): config_file=%s qos=%s",
    app_config.infer2.config_file ? app_config.infer2.config_file : "(disabled)",
    app_config.infer2.qos ? "TRUE" : "FALSE");
  g_print("\n  tracker: width=%u height=%u ll_lib_file=%s ll_config_file=%s display_tracking_id=%s",
    app_config.tracker.width,
    app_config.tracker.height,
    app_config.tracker.ll_lib_file ? app_config.tracker.ll_lib_file : "(none)",
    app_config.tracker.ll_config_file ? app_config.tracker.ll_config_file : "(none)",
    app_config.tracker.display_tracking_id ? "TRUE" : "FALSE");
  g_print("\n  osd: process_mode=%d qos=%s draw_landmarks=%s draw_custom_bbox=%s",
    app_config.osd.process_mode,
    app_config.osd.qos ? "TRUE" : "FALSE",
    app_config.osd.draw_landmarks ? "TRUE" : "FALSE",
    app_config.osd.draw_custom_bbox ? "TRUE" : "FALSE");
  g_print("\n  display: disabled=%s window_width=%u window_height=%u sync=%s async_sink=%s qos=%s",
    app_config.display.disabled ? "TRUE" : "FALSE",
    app_config.display.window_width,
    app_config.display.window_height,
    app_config.display.sync ? "TRUE" : "FALSE",
    app_config.display.async_sink ? "TRUE" : "FALSE",
    app_config.display.qos ? "TRUE" : "FALSE");
  g_print("\n  queue: max_size_buffers=%u leaky=%d",
    app_config.queue.max_size_buffers,
    app_config.queue.leaky);
  g_print("\n  appsink: max_buffers=%u drop=%s sync=%s",
    app_config.appsink.max_buffers,
    app_config.appsink.drop ? "TRUE" : "FALSE",
    app_config.appsink.sync ? "TRUE" : "FALSE");
  g_print("\n  kafka: enabled=%s broker=%s topic=%s event_topic=%s send_delay_sec=%.3f quality_improvement_threshold=%.3f sent_record_ttl_sec=%.3f pending_ttl_sec=%.3f cleanup_interval_sec=%.3f",
    app_config.kafka.enabled ? "TRUE" : "FALSE",
    app_config.kafka.broker ? app_config.kafka.broker : "(none)",
    app_config.kafka.topic ? app_config.kafka.topic : "(none)",
    app_config.kafka.event_topic ? app_config.kafka.event_topic : "(same as topic)",
    app_config.kafka.send_delay_sec,
    app_config.kafka.quality_improvement_threshold,
    app_config.kafka.sent_record_ttl_sec,
    app_config.kafka.pending_ttl_sec,
    app_config.kafka.cleanup_interval_sec);
  g_print("\n  face_quality: min_landmark_confidence=%.3f min_visible_landmarks=%u face_quality_threshold=%.3f min_frontal_score=%.3f",
    app_config.face_quality.min_landmark_confidence,
    app_config.face_quality.min_visible_landmarks,
    app_config.face_quality.face_quality_threshold,
    app_config.face_quality.min_frontal_score);
  g_print("\n  osd_text: max_display_len=%d ntp_text_x_offset=%d ntp_text_y_offset=%d ntp_text_font_size=%d",
    app_config.osd_text.max_display_len,
    app_config.osd_text.ntp_text_x_offset,
    app_config.osd_text.ntp_text_y_offset,
    app_config.osd_text.ntp_text_font_size);
  g_print("\n  frame_save: enabled=%s dir=%s quality=%u exclude_letterbox=%s"
          " save_all_frames=%s pre_buffer_duration=%.2f",
    app_config.frame_save.enabled ? "TRUE" : "FALSE",
    app_config.frame_save.dir ? app_config.frame_save.dir : "(none)",
    app_config.frame_save.quality,
    app_config.frame_save.exclude_letterbox ? "TRUE" : "FALSE",
    app_config.frame_save.save_all_frames   ? "TRUE" : "FALSE",
    app_config.frame_save.pre_buffer_duration_sec);
  g_print("\n  smart_record: enabled=%s dir=%s file_prefix=%s cache_size_sec=%u default_duration_sec=%u container=%u",
    app_config.smart_record.enabled ? "TRUE" : "FALSE",
    app_config.smart_record.dir ? app_config.smart_record.dir : "(none)",
    app_config.smart_record.file_prefix ? app_config.smart_record.file_prefix : "(none)",
    app_config.smart_record.cache_size_sec,
    app_config.smart_record.default_duration_sec,
    app_config.smart_record.container);
  g_print("\n  json_save: enabled=%s dir=%s\n==========================\n\n",
    app_config.json_save.enabled ? "TRUE" : "FALSE",
    app_config.json_save.dir ? app_config.json_save.dir : "(none)");
}

// =============================================================================
// config_free — release all heap-allocated strings inside AppConfig
// =============================================================================

void
config_free(void)
{
  g_free(app_config.config_file);
  app_config.config_file = NULL;

  g_strfreev(app_config.source.uris);
  app_config.source.uris  = NULL;
  app_config.source.count = 0;

  g_free(app_config.infer.config_file);
  app_config.infer.config_file = NULL;

  g_free(app_config.infer2.config_file);
  app_config.infer2.config_file = NULL;

  g_free(app_config.tracker.ll_lib_file);
  app_config.tracker.ll_lib_file = NULL;

  g_free(app_config.tracker.ll_config_file);
  app_config.tracker.ll_config_file = NULL;

  g_free(app_config.kafka.broker);
  app_config.kafka.broker = NULL;

  g_free(app_config.kafka.topic);
  app_config.kafka.topic = NULL;

  g_free(app_config.kafka.event_topic);
  app_config.kafka.event_topic = NULL;

  g_free(app_config.frame_save.dir);
  app_config.frame_save.dir = NULL;

  g_free(app_config.json_save.dir);
  app_config.json_save.dir = NULL;

  g_free(app_config.smart_record.dir);
  app_config.smart_record.dir = NULL;

  g_free(app_config.smart_record.file_prefix);
  app_config.smart_record.file_prefix = NULL;
}