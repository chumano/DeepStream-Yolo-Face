#include "config.h"

// Global variable definitions
gchar *CONFIG_FILE = NULL;
//gchar *SOURCE = NULL;
gchar **SOURCES = NULL;
guint NUM_SOURCES = 0;

gchar *INFER_CONFIG = NULL;
guint STREAMMUX_BATCH_SIZE = 1;
guint STREAMMUX_WIDTH = 1920;
guint STREAMMUX_HEIGHT = 1080;
guint GPU_ID = 0;

guint PERF_MEASUREMENT_INTERVAL_SEC = 5;
gboolean JETSON = FALSE;

// Kafka settings
gchar *KAFKA_BROKER = NULL;
gchar *KAFKA_TOPIC = NULL;
gboolean KAFKA_ENABLED = FALSE;
gdouble KAFKA_SEND_DELAY_SEC = 2.0;
gdouble KAFKA_QUALITY_IMPROVEMENT_THRESHOLD = 0.005;
gdouble KAFKA_SENT_RECORD_TTL_SEC = 60.0;
gdouble KAFKA_PENDING_TTL_SEC = 10.0;
gdouble KAFKA_CLEANUP_INTERVAL_SEC = 30.0;

// Face quality thresholds
gdouble MIN_LANDMARK_CONFIDENCE = 0.5;
guint MIN_VISIBLE_LANDMARKS = 3;
gdouble FACE_QUALITY_THRESHOLD = 0.6;
//gdouble MAX_HEAD_ROTATION_ANGLE = 25.0;
gdouble MIN_FRONTAL_SCORE = 0.7;

// Crop image support
gboolean ENABLE_CROP_IMAGE = TRUE;

// Display settings
gboolean DISABLE_DISPLAY = FALSE;

// Frame saving settings
gboolean ENABLE_FRAME_SAVE = TRUE;
gchar *FRAME_SAVE_DIR = NULL;
guint FRAME_SAVE_QUALITY = 70; // JPEG quality (0-100)

int MAX_DISPLAY_LEN = 128;
int NTP_TEXT_X_OFFSET = 30;
int NTP_TEXT_Y_OFFSET = 30;
int NTP_TEXT_FONT_SIZE = 20;

gboolean WAIT_FOR_USER_INPUT = TRUE;

// Triton / nvinferserver
gboolean USE_TRITON = FALSE;

gboolean
parse_config_file(const gchar *config_file, GError **error)
{
  GKeyFile *keyfile = g_key_file_new();
  
  if (!g_key_file_load_from_file(keyfile, config_file, G_KEY_FILE_NONE, error)) {
    g_key_file_free(keyfile);
    return FALSE;
  }
  
  GError *key_error = NULL;
  
  // Helper macro to safely get string values
  #define GET_STRING(key, var) \
    do { \
      gchar *val = g_key_file_get_string(keyfile, "settings", key, &key_error); \
      if (val && !key_error) { \
        if (var) g_free(var); \
        var = val; \
      } else if (key_error) { \
        g_clear_error(&key_error); \
      } \
    } while(0)
  
  // Helper macro to safely get string array values
  #define GET_STRING_ARRAY(key, var, num_var) \
    do { \
      gsize length = 0; \
      gchar **val = g_key_file_get_string_list(keyfile, "settings", key, &length, &key_error); \
      if (val && !key_error) { \
        if (var) g_strfreev(var); \
        var = val; \
        num_var = length; \
      } else if (key_error) { \
        g_clear_error(&key_error); \
      } \
    } while(0)
  
  // Helper macro to safely get integer values
  #define GET_INT(key, var) \
    do { \
      gint val = g_key_file_get_integer(keyfile, "settings", key, &key_error); \
      if (!key_error) { \
        var = val; \
      } else { \
        g_clear_error(&key_error); \
      } \
    } while(0)
  
  // Helper macro to safely get double values
  #define GET_DOUBLE(key, var) \
    do { \
      gdouble val = g_key_file_get_double(keyfile, "settings", key, &key_error); \
      if (!key_error) { \
        var = val; \
      } else { \
        g_clear_error(&key_error); \
      } \
    } while(0)
  
  // Helper macro to safely get boolean values
  #define GET_BOOLEAN(key, var) \
    do { \
      gboolean val = g_key_file_get_boolean(keyfile, "settings", key, &key_error); \
      if (!key_error) { \
        var = val; \
      } else { \
        g_clear_error(&key_error); \
      } \
    } while(0)
  
  // Load basic settings
  GET_STRING_ARRAY("sources", SOURCES, NUM_SOURCES);
  GET_STRING("infer-config", INFER_CONFIG);
  GET_INT("streammux-batch-size", STREAMMUX_BATCH_SIZE);
  GET_INT("streammux-width", STREAMMUX_WIDTH);
  GET_INT("streammux-height", STREAMMUX_HEIGHT);
  GET_INT("gpu-id", GPU_ID);
  GET_INT("perf-measurement-interval", PERF_MEASUREMENT_INTERVAL_SEC);
  
  // Load Kafka settings
  GET_STRING("kafka-broker", KAFKA_BROKER);
  GET_STRING("kafka-topic", KAFKA_TOPIC);
  GET_DOUBLE("kafka-delay", KAFKA_SEND_DELAY_SEC);
  GET_DOUBLE("kafka-quality-threshold", KAFKA_QUALITY_IMPROVEMENT_THRESHOLD);
  GET_DOUBLE("kafka-sent-record-ttl", KAFKA_SENT_RECORD_TTL_SEC);
  GET_DOUBLE("kafka-pending-ttl", KAFKA_PENDING_TTL_SEC);
  GET_DOUBLE("kafka-cleanup-interval", KAFKA_CLEANUP_INTERVAL_SEC);
  
  // Load face quality thresholds
  GET_DOUBLE("min-landmark-confidence", MIN_LANDMARK_CONFIDENCE);
  GET_INT("min-visible-landmarks", MIN_VISIBLE_LANDMARKS);
  GET_DOUBLE("face-quality-threshold", FACE_QUALITY_THRESHOLD);
  GET_DOUBLE("min-frontal-score", MIN_FRONTAL_SCORE);
  
  // Load crop and display settings
  GET_BOOLEAN("enable-crop-image", ENABLE_CROP_IMAGE);
  GET_BOOLEAN("disable-display", DISABLE_DISPLAY);
  
  // Load frame saving settings
  GET_BOOLEAN("enable-frame-save", ENABLE_FRAME_SAVE);
  GET_STRING("frame-save-dir", FRAME_SAVE_DIR);
  GET_INT("frame-save-quality", FRAME_SAVE_QUALITY);
  
  // Load misc settings
  GET_BOOLEAN("wait-for-user-input", WAIT_FOR_USER_INPUT);
  GET_BOOLEAN("use-triton", USE_TRITON);
  
  #undef GET_STRING
  #undef GET_STRING_ARRAY
  #undef GET_INT
  #undef GET_DOUBLE
  #undef GET_BOOLEAN
  
  g_key_file_free(keyfile);
  return TRUE;
}

GOptionEntry entries[] = {
  {"config", 'f', 0, G_OPTION_ARG_STRING, &CONFIG_FILE, "Configuration file", NULL},
  {"source", 's', 0, G_OPTION_ARG_STRING_ARRAY, &SOURCES, "Source streams/files (can specify multiple -s)", NULL},
  {"infer-config", 'c', 0, G_OPTION_ARG_STRING, &INFER_CONFIG, "Config infer file", NULL},
  {"streammux-batch-size", 'b', 0, G_OPTION_ARG_INT, &STREAMMUX_BATCH_SIZE, "Streammux batch-size (default 1)", NULL},
  {"streammux-width", 'w', 0, G_OPTION_ARG_INT, &STREAMMUX_WIDTH, "Streammux width (default 1920)", NULL},
  {"streammux-height", 'e', 0, G_OPTION_ARG_INT, &STREAMMUX_HEIGHT, "Streammux height (default 1080)", NULL},
  {"gpu-id", 'g', 0, G_OPTION_ARG_INT, &GPU_ID, "GPU id (default 0)", NULL},
  {"kafka-broker", 'k', 0, G_OPTION_ARG_STRING, &KAFKA_BROKER, "Kafka broker address (e.g., localhost:9092)", NULL},
  {"kafka-topic", 't', 0, G_OPTION_ARG_STRING, &KAFKA_TOPIC, "Kafka topic name (default: face-detections)", NULL},
  {"kafka-delay", 'd', 0, G_OPTION_ARG_DOUBLE, &KAFKA_SEND_DELAY_SEC, "Delay in seconds before sending to Kafka (default: 2.0)", NULL},
  {"kafka-quality-threshold", 'q', 0, G_OPTION_ARG_DOUBLE, &KAFKA_QUALITY_IMPROVEMENT_THRESHOLD, "Minimum quality improvement to resend (default: 0.005)", NULL},
  {"disable-crop-image", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &ENABLE_CROP_IMAGE, "Disable crop image in Kafka JSON", NULL},
  {"disable-display", 0, 0, G_OPTION_ARG_NONE, &DISABLE_DISPLAY, "Disable video display output", NULL},
  {"use-triton", 0, 0, G_OPTION_ARG_NONE, &USE_TRITON, "Use nvinferserver (Triton) instead of nvinfer", NULL},
  {"enable-frame-save", 0, 0, G_OPTION_ARG_NONE, &ENABLE_FRAME_SAVE, "Enable saving frames to disk", NULL},
  {"frame-save-dir", 0, 0, G_OPTION_ARG_STRING, &FRAME_SAVE_DIR, "Directory to save frames (default: ./outputs/frames)", NULL},
  {"frame-save-quality", 0, 0, G_OPTION_ARG_INT, &FRAME_SAVE_QUALITY, "JPEG quality 0-100 (default: 85)", NULL},
  {NULL}
};

gint parse_command_line(gint argc, char *argv[])
{
  // ========== PASS 1: Look for config file ==========
  for (gint i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "--config") == 0 || g_strcmp0(argv[i], "-f") == 0) {
      if (i + 1 < argc) {
        CONFIG_FILE = g_strdup(argv[i + 1]);
        g_print("Found config file: %s\n", CONFIG_FILE);
        break;
      }
    }
  }
  
  // ========== Load config file if specified ==========
  if (CONFIG_FILE) {
    g_printf("Loading configuration from file...\n");
    GError *config_error = NULL;
    if (!parse_config_file(CONFIG_FILE, &config_error)) {
      g_printerr("ERROR - Failed to load config file: %s\n", 
                 config_error ? config_error->message : "unknown error");
      if(config_error) g_error_free(config_error);
      return -1;
    }
    g_print("Configuration loaded successfully\n");
  }

  // ========== Parse all command line options (override config) ==========
  g_print("Parsing command line options...\n");
 GOptionContext *ctx = g_option_context_new("DeepStream");
  GOptionGroup *group = g_option_group_new("deepstream", NULL, NULL, NULL, NULL);
  GError *error = NULL;
  g_option_group_add_entries(group, entries);
  g_option_context_set_main_group(ctx, group);
  g_option_context_add_group(ctx, gst_init_get_option_group());
  if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
    g_printerr("ERROR - %s\n", error->message);
    g_printerr("Run with --help to see available options\n");
    g_error_free(error);
    g_option_context_free(ctx);
    return -1;
  }
  g_option_context_free(ctx);

  // ========== Validate parsed options ==========
  if (SOURCES) {
    NUM_SOURCES = g_strv_length(SOURCES);
  }

  if (NUM_SOURCES == 0) {
    g_printerr("ERROR - No sources provided. Use -s <uri> to specify source(s)\n");
    return -1;
  }

  if (STREAMMUX_BATCH_SIZE < NUM_SOURCES) {
    STREAMMUX_BATCH_SIZE = NUM_SOURCES;
    g_print("Setting batch-size to %d to match number of sources\n", STREAMMUX_BATCH_SIZE);
  }

  if (!INFER_CONFIG) {
    g_printerr("ERROR - Config infer file not provided. Use -c <path> to specify\n");
    return -1;
  }

  // Check if Kafka is enabled
  if (KAFKA_BROKER) {
    KAFKA_ENABLED = TRUE;
    if (!KAFKA_TOPIC) {
      KAFKA_TOPIC = g_strdup("face-detections");
    }
  }

  g_printf("Command line options parsed successfully\n");
  return 1;
}

void config_free()
{
  // Free allocated global variables
  if (CONFIG_FILE) {
    g_free(CONFIG_FILE);
    CONFIG_FILE = NULL;
  }
  // if (SOURCE) {
  //   g_free(SOURCE);
  //   SOURCE = NULL;
  // }
  if (SOURCES) {
    g_strfreev(SOURCES);
    SOURCES = NULL;
    NUM_SOURCES = 0;
  }
  if (INFER_CONFIG) {
    g_free(INFER_CONFIG);
    INFER_CONFIG = NULL;
  }
  if (KAFKA_BROKER) {
    g_free(KAFKA_BROKER);
    KAFKA_BROKER = NULL;
  }
  if (KAFKA_TOPIC) {
    g_free(KAFKA_TOPIC);
    KAFKA_TOPIC = NULL;
  }
  if (FRAME_SAVE_DIR) {
    g_free(FRAME_SAVE_DIR);
    FRAME_SAVE_DIR = NULL;
  }
}