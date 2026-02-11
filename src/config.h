#ifndef CONFIG_H
#define CONFIG_H
#include <nvdsgstutils.h>

extern gchar *CONFIG_FILE;

// Basic settings
//extern gchar *SOURCE;
extern gchar **SOURCES;  // Array of source URIs
extern guint NUM_SOURCES;

extern gchar *INFER_CONFIG;
extern guint STREAMMUX_BATCH_SIZE;
extern guint STREAMMUX_WIDTH;
extern guint STREAMMUX_HEIGHT;
extern guint GPU_ID;

extern guint PERF_MEASUREMENT_INTERVAL_SEC;
extern gboolean JETSON;

// Kafka settings
extern gchar *KAFKA_BROKER;
extern gchar *KAFKA_TOPIC;
extern gboolean KAFKA_ENABLED;
extern gdouble KAFKA_SEND_DELAY_SEC;
extern gdouble KAFKA_QUALITY_IMPROVEMENT_THRESHOLD;
extern gdouble KAFKA_SENT_RECORD_TTL_SEC;
extern gdouble KAFKA_PENDING_TTL_SEC;
extern gdouble KAFKA_CLEANUP_INTERVAL_SEC;

// Face quality thresholds
extern gdouble MIN_LANDMARK_CONFIDENCE;
extern guint MIN_VISIBLE_LANDMARKS;
extern gdouble FACE_QUALITY_THRESHOLD;
//extern gdouble MAX_HEAD_ROTATION_ANGLE;
extern gdouble MIN_FRONTAL_SCORE;

// Crop image support
extern gboolean ENABLE_CROP_IMAGE;

// Display settings
extern gboolean DISABLE_DISPLAY;

// Frame saving settings
extern gboolean ENABLE_FRAME_SAVE;
extern gchar *FRAME_SAVE_DIR;
extern guint FRAME_SAVE_QUALITY; // JPEG quality (0-100)

extern int MAX_DISPLAY_LEN;
extern int NTP_TEXT_X_OFFSET;
extern int NTP_TEXT_Y_OFFSET;
extern int NTP_TEXT_FONT_SIZE;

extern gboolean WAIT_FOR_USER_INPUT;


gboolean
parse_config_file(const gchar *config_file, GError **error);

gint parse_command_line(gint argc, char *argv[]);
#endif // CONFIG_H