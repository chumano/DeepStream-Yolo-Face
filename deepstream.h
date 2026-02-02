#ifndef __DEEPSTREAM_H__
#define __DEEPSTREAM_H__

#include <nvdsgstutils.h>
#include <cuda_runtime_api.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "gstnvdsmeta.h"
#include "nvbufsurface.h"

#include "modules/interrupt.h"
#include "modules/perf.h"

// Kafka support with librdkafka
#ifdef KAFKA_ENABLED_BUILD
#include <librdkafka/rdkafka.h>
#endif

// Basic settings
static gchar *SOURCE = NULL;
static gchar *INFER_CONFIG = NULL;
static guint STREAMMUX_BATCH_SIZE = 1;
static guint STREAMMUX_WIDTH = 1920;
static guint STREAMMUX_HEIGHT = 1080;
static guint GPU_ID = 0;

static guint PERF_MEASUREMENT_INTERVAL_SEC = 5;
static gboolean JETSON = FALSE;

// Kafka settings
static gchar *KAFKA_BROKER = NULL;
static gchar *KAFKA_TOPIC = NULL;
static gboolean KAFKA_ENABLED = FALSE;
static gdouble KAFKA_SEND_DELAY_SEC = 2.0;
static gdouble KAFKA_QUALITY_IMPROVEMENT_THRESHOLD = 0.1;
static gdouble KAFKA_SENT_RECORD_TTL_SEC = 60.0;
static gdouble KAFKA_PENDING_TTL_SEC = 10.0;
static gdouble KAFKA_CLEANUP_INTERVAL_SEC = 30.0;

// Face quality thresholds
static gdouble MIN_LANDMARK_CONFIDENCE = 0.5;
static guint MIN_VISIBLE_LANDMARKS = 3;
static gdouble FACE_QUALITY_THRESHOLD = 0.6;
static gdouble MAX_HEAD_ROTATION_ANGLE = 25.0;
static gdouble MIN_FRONTAL_SCORE = 0.7;

// =============================================================================
// Detection Manager Structures
// =============================================================================

typedef struct _FaceQualityMetrics {
  gboolean is_frontal;
  guint visible_landmarks;
  guint total_landmarks;
  gdouble avg_confidence;
  gdouble quality_score;
  gdouble frontal_score;
} FaceQualityMetrics;

typedef struct _Landmark {
  gdouble x;
  gdouble y;
  gdouble confidence;
} Landmark;

typedef struct _Detection {
  guint64 object_id;
  gdouble quality_score;
  gdouble timestamp;
  gboolean is_resend;
  gchar *json_data;
} Detection;

typedef struct _DetectionRecord {
  guint64 object_id;
  gdouble quality_score;
  gdouble sent_timestamp;
  guint send_count;
  gdouble last_seen_timestamp;
} DetectionRecord;

typedef struct _DetectionStore {
  GHashTable *pending;      // object_id -> Detection*
  GHashTable *sent;         // object_id -> DetectionRecord*
  pthread_mutex_t pending_lock;
  pthread_mutex_t sent_lock;
  gdouble sent_record_ttl_sec;
  gdouble pending_ttl_sec;
} DetectionStore;

typedef struct _DetectionStats {
  guint queued;
  guint sent;
  guint skipped;
  guint failed;
  guint resent;
  guint cleaned_sent;
  guint cleaned_pending;
  pthread_mutex_t lock;
} DetectionStats;

typedef struct _DetectionManager {
  gboolean enabled;
  gchar *broker;
  gchar *topic;
  gdouble delay_sec;
  gdouble quality_improvement_threshold;
  gdouble cleanup_interval_sec;
  gdouble last_cleanup_time;
  DetectionStore *store;
  DetectionStats stats;
#ifdef KAFKA_ENABLED_BUILD
  rd_kafka_t *kafka_producer;
  rd_kafka_topic_t *kafka_topic;
#else
  void *kafka_producer;  // Placeholder when librdkafka is not available
  void *kafka_topic;
#endif
} DetectionManager;

// Global detection manager
static DetectionManager *detection_manager = NULL;

#endif
