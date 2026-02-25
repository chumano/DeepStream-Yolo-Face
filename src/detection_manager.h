#ifndef __DETECTION_MANAGER_H__
#define __DETECTION_MANAGER_H__

#include <glib.h>
#include <pthread.h>

#ifdef KAFKA_ENABLED_BUILD
#include <librdkafka/rdkafka.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Type Definitions
// =============================================================================

typedef struct _DetectionManager DetectionManager;

// =============================================================================
// Public API
// =============================================================================

/**
 * Create a new detection manager instance
 * @param enabled Whether the manager is enabled
 * @param broker Kafka broker address (can be NULL if not using Kafka)
 * @param topic Kafka topic name (can be NULL if not using Kafka)
 * @param delay_sec Delay before sending detection in seconds
 * @param quality_threshold Quality improvement threshold for resending
 * @param cleanup_interval_sec Interval for cleanup in seconds
 * @param sent_record_ttl_sec TTL for sent records in seconds
 * @param pending_ttl_sec TTL for pending detections in seconds
 * @return New DetectionManager instance or NULL on failure
 */
DetectionManager* detection_manager_new(gboolean enabled,
                                        const gchar *broker,
                                        const gchar *topic,
                                        gdouble delay_sec,
                                        gdouble quality_threshold,
                                        gdouble cleanup_interval_sec,
                                        gdouble sent_record_ttl_sec,
                                        gdouble pending_ttl_sec);

/**
 * Free detection manager and all resources
 * @param manager Detection manager instance
 */
void detection_manager_free(DetectionManager *manager);

/**
 * Queue a detection for sending
 * @param manager Detection manager instance
 * @param source_id Source identifier
 * @param object_id Object identifier
 * @param quality_score Face quality score
 * @param json_data JSON payload to send
 */
void detection_manager_queue(DetectionManager *manager,
                             guint source_id,
                             guint64 object_id,
                             gdouble quality_score,
                             const gchar *json_data);

/**
 * Process pending detections and send ready ones
 * @param manager Detection manager instance
 * @return Number of detections sent
 */
guint detection_manager_process_pending(DetectionManager *manager);

/**
 * Print statistics about detections
 * @param manager Detection manager instance
 */
void detection_manager_print_stats(DetectionManager *manager);

/**
 * Initialize Kafka producer (only if KAFKA_ENABLED_BUILD is defined)
 * @param manager Detection manager instance
 * @return TRUE on success, FALSE on failure
 */
gboolean detection_manager_init_kafka(DetectionManager *manager);

/**
 * Cleanup Kafka resources
 * @param manager Detection manager instance
 */
void detection_manager_cleanup_kafka(DetectionManager *manager);

/**
 * Check if detection manager is enabled
 * @param manager Detection manager instance
 * @return TRUE if enabled, FALSE otherwise
 */
gboolean detection_manager_is_enabled(DetectionManager *manager);

#ifdef __cplusplus
}
#endif

#endif // __DETECTION_MANAGER_H__
