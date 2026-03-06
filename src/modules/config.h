#ifndef __CONFIG_H__
#define __CONFIG_H__
#include <nvdsgstutils.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Configuration structs — one per pipeline / feature domain
// =============================================================================

/** Input source URIs */
typedef struct {
  gchar **uris;   /**< NULL-terminated array of URI strings */
  guint   count;  /**< Number of active sources */
} AppSourceConfig;

/** nvstreammux element */
typedef struct {
  guint batch_size;             /**< default 1 */
  guint width;                  /**< default 1920 */
  guint height;                 /**< default 1080 */
  gboolean enable_padding;      /**< default FALSE */
  guint batched_push_timeout;   /**< microseconds, default 25000 */
} AppStreamMuxConfig;

/** nvinfer / nvinferserver element */
typedef struct {
  gchar    *config_file;  /**< path to nvinfer config txt */
  gboolean  use_triton;   /**< TRUE → nvinferserver, FALSE → nvinfer */
  gboolean  qos;          /**< default FALSE */
} AppInferConfig;

/** nvtracker element */
typedef struct {
  guint     width;                 /**< default 640 */
  guint     height;                /**< default 384 */
  gchar    *ll_lib_file;           /**< low-level tracker .so */
  gchar    *ll_config_file;        /**< tracker config YAML */
  gboolean  display_tracking_id;   /**< default TRUE */
} AppTrackerConfig;

/** nvdsosd element */
typedef struct {
  gint     process_mode;  /**< 0=CPU, 1=GPU; default 1 (MODE_GPU) */
  gboolean qos;           /**< default FALSE */
  gboolean draw_landmarks;        /**< TRUE → draw landmark circles on OSD; default TRUE */
  gboolean draw_custom_bbox;      /**< TRUE → apply custom bbox style; default TRUE */
} AppOsdConfig;

/** Display sink (nveglglessink / nv3dsink) */
typedef struct {
  gboolean disabled;       /**< TRUE → use fakesink instead */
  guint    window_width;   /**< default 400 */
  guint    window_height;  /**< default 400 */
  gboolean sync;           /**< default FALSE */
  gboolean async_sink;     /**< default FALSE */
  gboolean qos;            /**< default FALSE */
} AppDisplayConfig;

/** Shared queue element settings (queue_display and queue_app) */
typedef struct {
  guint max_size_buffers;  /**< default 5 */
  gint  leaky;             /**< 0=no-leak, 1=upstream, 2=downstream; default 2 */
} AppQueueConfig;

/** appsink element */
typedef struct {
  guint    max_buffers;  /**< default 5 */
  gboolean drop;         /**< default TRUE */
  gboolean sync;         /**< default FALSE */
} AppAppsinkConfig;

/** Kafka / detection-manager */
typedef struct {
  gboolean enabled;
  gchar   *broker;
  gchar   *topic;
  gdouble  send_delay_sec;
  gdouble  quality_improvement_threshold;
  gdouble  sent_record_ttl_sec;
  gdouble  pending_ttl_sec;
  gdouble  cleanup_interval_sec;
} AppKafkaConfig;

/** Face quality assessment thresholds */
typedef struct {
  gdouble min_landmark_confidence;
  guint   min_visible_landmarks;
  gdouble face_quality_threshold;
  gdouble min_frontal_score;
} AppFaceQualityConfig;

/** On-screen text overlay (NTP timestamp + object ID labels) */
typedef struct {
  gint max_display_len;
  gint ntp_text_x_offset;
  gint ntp_text_y_offset;
  gint ntp_text_font_size;
} AppOsdTextConfig;

/** JPEG frame-save */
typedef struct {
  gboolean enabled;
  gchar   *dir;
  guint    quality;  /**< JPEG quality 0–100 */
  gboolean exclude_letterbox; /**< if TRUE, crop out letterbox padding before saving */

  /** Saving mode */
  gboolean save_all_frames;         /**< TRUE → save every frame unconditionally;
                                         FALSE → smart-save (only on detection) */
  gdouble  pre_buffer_duration_sec; /**< seconds of frames to keep in the buffer
                                         (default 1.0) */
} AppFrameSaveConfig;

/** Per-frame JSON output */
typedef struct {
  gboolean enabled;   /**< TRUE → write one JSON file per inferred frame */
  gchar   *dir;       /**< output directory, e.g. /app/outputs/json */
} AppJsonSaveConfig;

// =============================================================================
// Top-level application configuration
// =============================================================================

typedef struct {
  gchar   *config_file;  /**< path to INI config (if provided via -f) */

  /* General */
  guint    gpu_id;
  gboolean jetson;
  guint    perf_measurement_interval_sec;
  gboolean wait_for_user_input;
  gboolean enable_crop_image;

  /* Per-domain structs */
  AppSourceConfig      source;
  AppStreamMuxConfig   streammux;
  AppInferConfig       infer;   /**< primary detector (nvinfer or nvinferserver) */
  AppInferConfig       infer2;  /**< secondary Triton detector; disabled when config_file == NULL */
  AppTrackerConfig     tracker;
  AppOsdConfig         osd;
  AppDisplayConfig     display;
  AppQueueConfig       queue;
  AppAppsinkConfig     appsink;
  AppKafkaConfig       kafka;
  AppFaceQualityConfig face_quality;
  AppOsdTextConfig     osd_text;
  AppFrameSaveConfig   frame_save;
  AppJsonSaveConfig    json_save;
} AppConfig;

/** Single global instance — defined in config.c */
extern AppConfig app_config;

// =============================================================================
// Functions
// =============================================================================

gboolean parse_config_file(const gchar *config_file, GError **error);
gint     parse_command_line(gint argc, char *argv[]);
void     config_free(void);

#ifdef __cplusplus
}
#endif

#endif // __CONFIG_H__