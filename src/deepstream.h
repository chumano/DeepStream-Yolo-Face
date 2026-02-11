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

// =============================================================================
// Detection Manager Structures
// =============================================================================

typedef struct {
  guint left;
  guint top;
  guint width;
  guint height;
} CropBox;

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

typedef struct _FaceContext {
  guint source_id;
  gdouble frame_timestamp;
  guint frame_num;
  guint64 object_id;
  gint class_id;
  gdouble confidence;

  const Landmark *landmarks;
  guint num_landmarks;
  const gchar* frame_image_path;

  const CropBox* bbox;
  const CropBox* crop_box;
  gboolean is_good_face;
  gdouble quality_score;
  const FaceQualityMetrics *metrics;
  const gchar *face_image_base64;
} FaceContext;

// Composite key for tracking detections across multiple sources
typedef struct _DetectionKey {
  guint source_id;
  guint64 object_id;
} DetectionKey;

typedef struct _Detection {
  guint source_id;           // Added: source identifier
  guint64 object_id;
  gdouble quality_score;
  gdouble timestamp;
  gboolean is_resend;
  gchar *json_data;
} Detection;

typedef struct _DetectionRecord {
  guint source_id;           // Added: source identifier
  guint64 object_id;
  gdouble quality_score;
  gdouble sent_timestamp;
  guint send_count;
  gdouble last_seen_timestamp;
} DetectionRecord;

typedef struct _DetectionStore {
  GHashTable *pending;      // DetectionKey* -> Detection*
  GHashTable *sent;         // DetectionKey* -> DetectionRecord*
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
