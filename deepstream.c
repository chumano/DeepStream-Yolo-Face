#include "deepstream.h"
#include <jpeglib.h>
#include <setjmp.h>
#include "nvbufsurftransform.h"

GST_DEBUG_CATEGORY_STATIC(deepstream_debug_category);
#define GST_CAT_DEFAULT deepstream_debug_category

GOptionEntry entries[] = {
  {"source", 's', 0, G_OPTION_ARG_STRING_ARRAY, &SOURCES, "Source streams/files (can specify multiple -s)", NULL},
 // {"source", 's', 0, G_OPTION_ARG_STRING, &SOURCE, "Source stream/file", NULL},
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
  {"enable-frame-save", 0, 0, G_OPTION_ARG_NONE, &ENABLE_FRAME_SAVE, "Enable saving frames to disk", NULL},
  {"frame-save-dir", 0, 0, G_OPTION_ARG_STRING, &FRAME_SAVE_DIR, "Directory to save frames (default: ./outputs/frames)", NULL},
  {"frame-save-quality", 0, 0, G_OPTION_ARG_INT, &FRAME_SAVE_QUALITY, "JPEG quality 0-100 (default: 85)", NULL},
  {NULL}
};

// =============================================================================
// Utility Functions
// =============================================================================

// monotonic time
static gdouble
get_current_time(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (gdouble) ts.tv_sec + (gdouble) ts.tv_nsec / 1000000000.0;
}

// =============================================================================
// Detection Store Functions
// =============================================================================

static void
detection_free(Detection *detection)
{
  if (detection) {
    if (detection->json_data) {
      g_free(detection->json_data);
    }
    g_free(detection);
  }
}

static void
detection_record_free(DetectionRecord *record)
{
  if (record) {
    g_free(record);
  }
}

static DetectionStore *
detection_store_new(gdouble sent_record_ttl_sec, gdouble pending_ttl_sec)
{
  DetectionStore *store = g_malloc0(sizeof(DetectionStore));
  store->pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify) detection_free);
  store->sent = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify) detection_record_free);
  pthread_mutex_init(&store->pending_lock, NULL);
  pthread_mutex_init(&store->sent_lock, NULL);
  store->sent_record_ttl_sec = sent_record_ttl_sec;
  store->pending_ttl_sec = pending_ttl_sec;
  return store;
}

static void
detection_store_free(DetectionStore *store)
{
  if (store) {
    pthread_mutex_destroy(&store->pending_lock);
    pthread_mutex_destroy(&store->sent_lock);
    g_hash_table_destroy(store->pending);
    g_hash_table_destroy(store->sent);
    g_free(store);
  }
}

static Detection *
detection_store_get_pending(DetectionStore *store, guint64 object_id)
{
  Detection *result = NULL;
  pthread_mutex_lock(&store->pending_lock);
  result = g_hash_table_lookup(store->pending, &object_id);
  pthread_mutex_unlock(&store->pending_lock);
  return result;
}

static void
detection_store_set_pending(DetectionStore *store, Detection *detection)
{
  pthread_mutex_lock(&store->pending_lock);
  guint64 *key = g_malloc(sizeof(guint64));
  *key = detection->object_id;
  g_hash_table_replace(store->pending, key, detection);
  pthread_mutex_unlock(&store->pending_lock);
}

static Detection *
detection_store_remove_pending(DetectionStore *store, guint64 object_id)
{
  Detection *result = NULL;
  pthread_mutex_lock(&store->pending_lock);
  result = g_hash_table_lookup(store->pending, &object_id);
  if (result) {
    g_hash_table_steal(store->pending, &object_id);
  }
  pthread_mutex_unlock(&store->pending_lock);
  return result;
}

static DetectionRecord *
detection_store_get_sent(DetectionStore *store, guint64 object_id)
{
  DetectionRecord *result = NULL;
  pthread_mutex_lock(&store->sent_lock);
  result = g_hash_table_lookup(store->sent, &object_id);
  pthread_mutex_unlock(&store->sent_lock);
  return result;
}

static void
detection_store_record_sent(DetectionStore *store, Detection *detection)
{
  pthread_mutex_lock(&store->sent_lock);
  
  DetectionRecord *existing = g_hash_table_lookup(store->sent, &detection->object_id);
  if (existing) {
    existing->quality_score = detection->quality_score;
    existing->sent_timestamp = get_current_time();
    existing->send_count++;
    existing->last_seen_timestamp = existing->sent_timestamp;
  } else {
    guint64 *key = g_malloc(sizeof(guint64));
    *key = detection->object_id;
    DetectionRecord *record = g_malloc0(sizeof(DetectionRecord));
    record->object_id = detection->object_id;
    record->quality_score = detection->quality_score;
    record->sent_timestamp = get_current_time();
    record->send_count = 1;
    record->last_seen_timestamp = record->sent_timestamp;
    g_hash_table_replace(store->sent, key, record);
  }
  
  pthread_mutex_unlock(&store->sent_lock);
}

static void
detection_store_update_last_seen(DetectionStore *store, guint64 object_id)
{
  pthread_mutex_lock(&store->sent_lock);
  DetectionRecord *record = g_hash_table_lookup(store->sent, &object_id);
  if (record) {
    record->last_seen_timestamp = get_current_time();
  }
  pthread_mutex_unlock(&store->sent_lock);
}

static void
detection_store_cleanup_stale_records(DetectionStore *store, guint *removed_sent, guint *removed_pending)
{
  gdouble current_time = get_current_time();
  *removed_sent = 0;
  *removed_pending = 0;
  
  // Cleanup sent records
  pthread_mutex_lock(&store->sent_lock);
  GHashTableIter iter;
  gpointer key, value;
  GList *keys_to_remove = NULL;
  
  g_hash_table_iter_init(&iter, store->sent);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    DetectionRecord *record = (DetectionRecord *) value;
    if ((current_time - record->last_seen_timestamp) > store->sent_record_ttl_sec) {
      keys_to_remove = g_list_prepend(keys_to_remove, key);
    }
  }
  
  for (GList *l = keys_to_remove; l != NULL; l = l->next) {
    g_hash_table_remove(store->sent, l->data);
    (*removed_sent)++;
  }
  g_list_free(keys_to_remove);
  pthread_mutex_unlock(&store->sent_lock);
  
  // Cleanup pending detections
  pthread_mutex_lock(&store->pending_lock);
  keys_to_remove = NULL;
  
  g_hash_table_iter_init(&iter, store->pending);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    Detection *detection = (Detection *) value;
    if ((current_time - detection->timestamp) > store->pending_ttl_sec) {
      keys_to_remove = g_list_prepend(keys_to_remove, key);
    }
  }
  
  for (GList *l = keys_to_remove; l != NULL; l = l->next) {
    g_hash_table_remove(store->pending, l->data);
    (*removed_pending)++;
  }
  g_list_free(keys_to_remove);
  pthread_mutex_unlock(&store->pending_lock);
}

// =============================================================================
// Face Quality Assessment
// =============================================================================

static gboolean
assess_face_quality(Landmark *landmarks, guint num_landmarks, 
                    gboolean *is_good_face, gdouble *quality_score, 
                    FaceQualityMetrics *metrics)
{
  if (!landmarks || num_landmarks < 5) {
    *is_good_face = FALSE;
    *quality_score = 0.0;
    return FALSE;
  }
  
  // Count visible landmarks
  guint visible_count = 0;
  gdouble confidence_sum = 0.0;
  
  for (guint i = 0; i < num_landmarks; i++) {
    if (landmarks[i].confidence >= MIN_LANDMARK_CONFIDENCE) {
      visible_count++;
      confidence_sum += landmarks[i].confidence;
    }
  }
  
  if (visible_count < MIN_VISIBLE_LANDMARKS) {
    *is_good_face = FALSE;
    *quality_score = 0.0;
    if (metrics) {
      metrics->is_frontal = FALSE;
      metrics->visible_landmarks = visible_count;
      metrics->total_landmarks = num_landmarks;
      metrics->avg_confidence = 0.0;
      metrics->quality_score = 0.0;
      metrics->frontal_score = 0.0;
    }
    return FALSE;
  }
  
  gdouble avg_confidence = confidence_sum / visible_count;
  
  // Extract key landmarks (5-point: left_eye, right_eye, nose, left_mouth, right_mouth)
  Landmark *left_eye = &landmarks[0];
  Landmark *right_eye = &landmarks[1];
  Landmark *nose = &landmarks[2];
  
  gboolean is_frontal = FALSE;
  gdouble frontal_score = 0.0;
  
  // Check if key landmarks are visible
  if (left_eye->confidence >= MIN_LANDMARK_CONFIDENCE &&
      right_eye->confidence >= MIN_LANDMARK_CONFIDENCE &&
      nose->confidence >= MIN_LANDMARK_CONFIDENCE) {
    
    // Calculate eye distance (baseline)
    gdouble eye_distance = sqrt(pow(right_eye->x - left_eye->x, 2) + 
                                pow(right_eye->y - left_eye->y, 2));
    
    if (eye_distance > 0) {
      // Calculate eye center
      gdouble eye_center_x = (left_eye->x + right_eye->x) / 2.0;
      
      // Calculate nose offset from eye center (horizontal)
      gdouble nose_offset_x = fabs(nose->x - eye_center_x);
      
      // Normalize by eye distance
      gdouble nose_offset_ratio = nose_offset_x / eye_distance;
      
      // Calculate frontal score (1.0 = perfectly frontal, 0.0 = profile)
      // Nose should be roughly centered between eyes for frontal face
      frontal_score = MAX(0.0, 1.0 - nose_offset_ratio * 2.0);
      
      is_frontal = (frontal_score >= MIN_FRONTAL_SCORE);
    }
  }
  
  // Calculate overall quality score
  *quality_score = (avg_confidence * 0.5) + (frontal_score * 0.5);
  
  if (metrics) {
    metrics->is_frontal = is_frontal;
    metrics->visible_landmarks = visible_count;
    metrics->total_landmarks = num_landmarks;
    metrics->avg_confidence = avg_confidence;
    metrics->quality_score = *quality_score;
    metrics->frontal_score = frontal_score;
  }
  
  // Good face criteria: sufficient landmarks, good quality, and frontal
  *is_good_face = (visible_count >= MIN_VISIBLE_LANDMARKS &&
                   *quality_score >= FACE_QUALITY_THRESHOLD &&
                   is_frontal);
  
  return TRUE;
}

// =============================================================================
// Detection Manager Functions
// =============================================================================

static DetectionManager *
detection_manager_new(gboolean enabled)
{
  DetectionManager *manager = g_malloc0(sizeof(DetectionManager));
  manager->enabled = enabled;
  manager->delay_sec = KAFKA_SEND_DELAY_SEC;
  manager->quality_improvement_threshold = KAFKA_QUALITY_IMPROVEMENT_THRESHOLD;
  manager->cleanup_interval_sec = KAFKA_CLEANUP_INTERVAL_SEC;
  manager->last_cleanup_time = get_current_time();
  manager->store = detection_store_new(KAFKA_SENT_RECORD_TTL_SEC, KAFKA_PENDING_TTL_SEC);
  pthread_mutex_init(&manager->stats.lock, NULL);
  return manager;
}

static void
detection_manager_free(DetectionManager *manager)
{
  if (manager) {
    if (manager->broker) {
      g_free(manager->broker);
    }
    if (manager->topic) {
      g_free(manager->topic);
    }
    if (manager->store) {
      detection_store_free(manager->store);
    }
    pthread_mutex_destroy(&manager->stats.lock);
    g_free(manager);
  }
}

static void
detection_manager_increment_stat(DetectionManager *manager, const gchar *stat)
{
  pthread_mutex_lock(&manager->stats.lock);
  if (g_strcmp0(stat, "queued") == 0) manager->stats.queued++;
  else if (g_strcmp0(stat, "sent") == 0) manager->stats.sent++;
  else if (g_strcmp0(stat, "skipped") == 0) manager->stats.skipped++;
  else if (g_strcmp0(stat, "failed") == 0) manager->stats.failed++;
  else if (g_strcmp0(stat, "resent") == 0) manager->stats.resent++;
  else if (g_strcmp0(stat, "cleaned_sent") == 0) manager->stats.cleaned_sent++;
  else if (g_strcmp0(stat, "cleaned_pending") == 0) manager->stats.cleaned_pending++;
  pthread_mutex_unlock(&manager->stats.lock);
}

static gboolean
detection_manager_should_queue(DetectionManager *manager, Detection *detection,
                                Detection *pending, DetectionRecord *sent_record)
{
  // If already sent, only allow if quality improves significantly
  if (sent_record != NULL) {
    gdouble improvement = detection->quality_score - sent_record->quality_score;
    if (improvement < manager->quality_improvement_threshold) {
      return FALSE;
    }
  }
  
  // If there's a pending detection, only replace if new one is better
  if (pending != NULL) {
    if (detection->quality_score <= pending->quality_score) {
      return FALSE;
    }
  }
  
  return TRUE;
}

static gboolean
detection_manager_should_send(DetectionManager *manager, Detection *detection, gdouble current_time)
{
  gdouble elapsed = current_time - detection->timestamp;
  return (elapsed >= manager->delay_sec);
}

// =============================================================================
// Kafka Functions
// =============================================================================

#ifdef KAFKA_ENABLED_BUILD
static void
kafka_delivery_report_cb(rd_kafka_t *rk, const rd_kafka_message_t *rkmessage, void *opaque)
{
  if (rkmessage->err) {
    g_printerr("ERROR - Kafka delivery failed: %s\n", rd_kafka_err2str(rkmessage->err));
  }
}

static void
kafka_error_cb(rd_kafka_t *rk, int err, const char *reason, void *opaque)
{
  g_printerr("ERROR - Kafka error: %s: %s\n", rd_kafka_err2str(err), reason);
}

static rd_kafka_t *
kafka_producer_create(const gchar *broker)
{
  rd_kafka_conf_t *conf = rd_kafka_conf_new();
  char errstr[512];
  
  // Set broker
  if (rd_kafka_conf_set(conf, "bootstrap.servers", broker, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    g_printerr("ERROR - Kafka config failed: %s\n", errstr);
    rd_kafka_conf_destroy(conf);
    return NULL;
  }
  
  // Set callbacks
  rd_kafka_conf_set_dr_msg_cb(conf, kafka_delivery_report_cb);
  rd_kafka_conf_set_error_cb(conf, kafka_error_cb);
  
  // Optional configurations for better performance
  rd_kafka_conf_set(conf, "queue.buffering.max.messages", "100000", NULL, 0);
  rd_kafka_conf_set(conf, "queue.buffering.max.ms", "100", NULL, 0);
  rd_kafka_conf_set(conf, "batch.num.messages", "1000", NULL, 0);
  
  // Create producer
  rd_kafka_t *producer = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
  if (!producer) {
    g_printerr("ERROR - Failed to create Kafka producer: %s\n", errstr);
    return NULL;
  }
  
  g_print("INFO - Kafka producer created for broker: %s\n", broker);
  return producer;
}

static rd_kafka_topic_t *
kafka_topic_create(rd_kafka_t *producer, const gchar *topic_name)
{
  rd_kafka_topic_conf_t *topic_conf = rd_kafka_topic_conf_new();
  
  rd_kafka_topic_t *topic = rd_kafka_topic_new(producer, topic_name, topic_conf);
  if (!topic) {
    g_printerr("ERROR - Failed to create Kafka topic: %s\n", rd_kafka_err2str(rd_kafka_last_error()));
    return NULL;
  }
  
  g_print("INFO - Kafka topic created: %s\n", topic_name);
  return topic;
}

static void
kafka_producer_destroy(rd_kafka_t *producer, rd_kafka_topic_t *topic)
{
  if (topic) {
    rd_kafka_topic_destroy(topic);
  }
  
  if (producer) {
    // Wait for outstanding messages to be delivered (max 5 seconds)
    g_print("INFO - Flushing Kafka producer...\n");
    rd_kafka_flush(producer, 5000);
    
    gint outq_len = rd_kafka_outq_len(producer);
    if (outq_len > 0) {
      g_printerr("WARNING - %d message(s) were not delivered\n", outq_len);
    }
    
    rd_kafka_destroy(producer);
    g_print("INFO - Kafka producer destroyed\n");
  }
}
#endif

static gboolean
detection_manager_send_kafka(DetectionManager *manager, Detection *detection)
{
#ifdef KAFKA_ENABLED_BUILD
  if (manager->kafka_producer == NULL || manager->kafka_topic == NULL) {
    GST_DEBUG("INFO - Kafka not connected, would send: object_id=%lu, quality=%.3f\n",
            detection->object_id, detection->quality_score);
    return TRUE;
  }
  // print 
  GST_INFO("INFO - Sending to Kafka: object_id=%lu, quality=%.3f, timestamp=%.3f\n",
          detection->object_id, detection->quality_score, detection->timestamp);

  // Send message to Kafka
  gint err = rd_kafka_produce(
      manager->kafka_topic,
      RD_KAFKA_PARTITION_UA,  // Use automatic partitioning
      RD_KAFKA_MSG_F_COPY,    // Copy the payload
      detection->json_data,
      strlen(detection->json_data),
      NULL, 0,  // No key
      NULL      // No opaque pointer
  );
  
  if (err == -1) {
    GST_ERROR("ERROR - Failed to produce message: %s\n", 
               rd_kafka_err2str(rd_kafka_last_error()));
    return FALSE;
  }
  
  // Poll for delivery reports (non-blocking)
  rd_kafka_poll(manager->kafka_producer, 0);
  
  return TRUE;
#else
  // Fallback when Kafka is not enabled
  if (manager->kafka_producer == NULL) {
    g_print("INFO - Kafka not compiled in, would send: object_id=%lu, quality=%.3f\n",
            detection->object_id, detection->quality_score);
    return TRUE;
  }
  return TRUE;
#endif
}

static void
detection_manager_queue(DetectionManager *manager, guint64 object_id,
                        gdouble quality_score, const gchar *json_data)
{
  if (!manager->enabled) {
    return;
  }
  
  gdouble current_time = get_current_time();
  DetectionRecord *sent_record = detection_store_get_sent(manager->store, object_id);
  gboolean is_resend = (sent_record != NULL);
  
  // Update last seen time if we have a sent record
  if (sent_record != NULL) {
    detection_store_update_last_seen(manager->store, object_id);
  }
  
  Detection *detection = g_malloc0(sizeof(Detection));
  detection->object_id = object_id;
  detection->quality_score = quality_score;
  detection->timestamp = current_time;
  detection->is_resend = is_resend;
  detection->json_data = g_strdup(json_data);
  
  Detection *pending = detection_store_get_pending(manager->store, object_id);
  
  if (!detection_manager_should_queue(manager, detection, pending, sent_record)) {
    detection_manager_increment_stat(manager, "skipped");
    detection_free(detection);
    return;
  }

  if( pending ) {
    GST_DEBUG("Replacing pending detection for object_id=%lu (old_quality=%.3f, new_quality=%.3f)\n",
             object_id, pending->quality_score, quality_score);
  }
  
  detection_store_set_pending(manager->store, detection);
  detection_manager_increment_stat(manager, "queued");
}

static guint
detection_manager_process_pending(DetectionManager *manager)
{
  if (!manager->enabled) {
    return 0;
  }
  
  gdouble current_time = get_current_time();
  
  // Periodic cleanup
  if ((current_time - manager->last_cleanup_time) >= manager->cleanup_interval_sec) {
    guint removed_sent, removed_pending;
    detection_store_cleanup_stale_records(manager->store, &removed_sent, &removed_pending);
    
    if (removed_sent > 0 || removed_pending > 0) {
      GST_INFO("INFO - Cleanup: removed %u sent records, %u pending detections\n",
              removed_sent, removed_pending);
      manager->stats.cleaned_sent += removed_sent;
      manager->stats.cleaned_pending += removed_pending;
    }
    manager->last_cleanup_time = current_time;
  }
  
  // Process pending detections
  guint sent_count = 0;
  
  pthread_mutex_lock(&manager->store->pending_lock);
  
  GHashTableIter iter;
  gpointer key, value;
  GList *ready_ids = NULL;
  
  g_hash_table_iter_init(&iter, manager->store->pending);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    Detection *detection = (Detection *) value;
    if (detection_manager_should_send(manager, detection, current_time)) {
      ready_ids = g_list_prepend(ready_ids, GUINT_TO_POINTER(detection->object_id));
    }
  }
  
  pthread_mutex_unlock(&manager->store->pending_lock);
  
  // Send ready detections
  for (GList *l = ready_ids; l != NULL; l = l->next) {
    guint64 object_id = GPOINTER_TO_UINT(l->data);
    Detection *detection = detection_store_remove_pending(manager->store, object_id);
    
    if (detection) {
      if (detection_manager_send_kafka(manager, detection)) {
        detection_store_record_sent(manager->store, detection);
        detection_manager_increment_stat(manager, "sent");
        if (detection->is_resend) {
          detection_manager_increment_stat(manager, "resent");
        }
        sent_count++;
      } else {
        detection_manager_increment_stat(manager, "failed");
      }
      detection_free(detection);
    }
  }
  
  g_list_free(ready_ids);
  return sent_count;
}

static gboolean
detection_manager_process_pending_callback(gpointer user_data)
{
  if (detection_manager) {
    detection_manager_process_pending(detection_manager);
  }
  return TRUE;
}

static void
detection_manager_print_stats(DetectionManager *manager)
{
  pthread_mutex_lock(&manager->stats.lock);
  g_print("INFO - Detection stats: queued=%u, sent=%u, skipped=%u, failed=%u, resent=%u, cleaned_sent=%u, cleaned_pending=%u\n",
          manager->stats.queued, manager->stats.sent, manager->stats.skipped,
          manager->stats.failed, manager->stats.resent, 
          manager->stats.cleaned_sent, manager->stats.cleaned_pending);
  pthread_mutex_unlock(&manager->stats.lock);
}

static void
init_detection_manager(void)
{
  if (!KAFKA_ENABLED) {
    detection_manager = detection_manager_new(FALSE);
    return;
  }
  
  detection_manager = detection_manager_new(TRUE);
  detection_manager->broker = g_strdup(KAFKA_BROKER);
  detection_manager->topic = g_strdup(KAFKA_TOPIC);
  
#ifdef KAFKA_ENABLED_BUILD
  // Initialize Kafka producer
  detection_manager->kafka_producer = kafka_producer_create(KAFKA_BROKER);
  if (detection_manager->kafka_producer) {
    detection_manager->kafka_topic = kafka_topic_create(detection_manager->kafka_producer, KAFKA_TOPIC);
    if (!detection_manager->kafka_topic) {
      GST_WARNING("WARNING - Failed to create Kafka topic, running without Kafka\n");
      kafka_producer_destroy(detection_manager->kafka_producer, NULL);
      detection_manager->kafka_producer = NULL;
    }
  } else {
    GST_WARNING("WARNING - Failed to create Kafka producer, running without Kafka\n");
  }
#else
  GST_WARNING("WARNING - Kafka support not compiled in. Build with KAFKA=1 to enable.\n");
  detection_manager->kafka_producer = NULL;
  detection_manager->kafka_topic = NULL;
#endif
  
  GST_DEBUG("Detection manager initialized (Kafka: %s, Topic: %s)\n",
          KAFKA_BROKER, KAFKA_TOPIC);
}

static void
cleanup_detection_manager(void)
{
  if (detection_manager) {
    detection_manager_print_stats(detection_manager);
    
#ifdef KAFKA_ENABLED_BUILD
    // Cleanup Kafka resources
    kafka_producer_destroy(detection_manager->kafka_producer, detection_manager->kafka_topic);
    detection_manager->kafka_producer = NULL;
    detection_manager->kafka_topic = NULL;
#endif
    
    detection_manager_free(detection_manager);
    detection_manager = NULL;
  }
}

// =============================================================================
// Image Processing Functions
// =============================================================================


static void
calculate_crop_box(NvDsObjectMeta *obj_meta, CropBox *crop_box, guint frame_width, guint frame_height)
{
  // Add 20% padding around the bounding box
  gfloat padding = 0.2f;
  
  gfloat pad_w = obj_meta->rect_params.width * padding;
  gfloat pad_h = obj_meta->rect_params.height * padding;
  
  gint left = (gint)(obj_meta->rect_params.left - pad_w);
  gint top = (gint)(obj_meta->rect_params.top - pad_h);
  gint right = (gint)(obj_meta->rect_params.left + obj_meta->rect_params.width + pad_w);
  gint bottom = (gint)(obj_meta->rect_params.top + obj_meta->rect_params.height + pad_h);
  
  // Clamp to frame boundaries
  crop_box->left = MAX(0, left);
  crop_box->top = MAX(0, top);
  crop_box->width = MIN(right, (gint)frame_width) - crop_box->left;
  crop_box->height = MIN(bottom, (gint)frame_height) - crop_box->top;
}

static gchar *
encode_crop_to_base64_jpeg(NvBufSurface *surface, CropBox *crop_box, gint quality, guint batch_id)
{
  if (!surface) {
    GST_ERROR("Surface is NULL");
    return NULL;
  }

  if (surface->numFilled < 1) {
    GST_ERROR("Surface has no filled buffers (numFilled=%d)", surface->numFilled);
    return NULL;
  }

  if (!surface->surfaceList) {
    GST_ERROR("Surface list is NULL");
    return NULL;
  }

  // Use batch_id instead of hardcoded 0
  if (batch_id >= surface->numFilled) {
    GST_ERROR("Invalid batch_id %u (numFilled=%d)", batch_id, surface->numFilled);
    return NULL;
  }

  NvBufSurfaceParams *surf_params = &surface->surfaceList[batch_id];

  // Additional validation
  if (!surf_params) {
    GST_ERROR("Surface params is NULL");
    return NULL;
  }

  if (surf_params->width == 0 || surf_params->height == 0) {
    GST_ERROR("Surface has invalid dimensions: %dx%d", surf_params->width, surf_params->height);
    return NULL;
  }

  GST_DEBUG("Surface info: width=%d, height=%d, pitch=%d, colorFormat=%d, memType=%d",
          surf_params->width, surf_params->height, surf_params->pitch,
          surface->surfaceList[batch_id].colorFormat, surface->memType);

  // Validate crop box
  if (crop_box->left >= surf_params->width || crop_box->top >= surf_params->height ||
      crop_box->width == 0 || crop_box->height == 0) {
    GST_ERROR("Invalid crop box: left=%u, top=%u, width=%u, height=%u (surface: %dx%d)",
               crop_box->left, crop_box->top, crop_box->width, crop_box->height,
               surf_params->width, surf_params->height);
    return NULL;
  }

  // Ensure crop doesn't exceed surface bounds
  if (crop_box->left + crop_box->width > surf_params->width) {
    crop_box->width = surf_params->width - crop_box->left;
  }
  if (crop_box->top + crop_box->height > surf_params->height) {
    crop_box->height = surf_params->height - crop_box->top;
  }

  if (crop_box->width < 10 || crop_box->height < 10) {
    GST_ERROR("Crop box too small: %ux%u", crop_box->width, crop_box->height);
    return NULL;
  }

  // Create destination surface for cropped RGBA image
  // Use CUDA_UNIFIED for transform compatibility, then copy to CPU
  NvBufSurface *dst_surface = NULL;
  NvBufSurfaceCreateParams create_params = {0};
  create_params.gpuId = surface->gpuId;
  create_params.width = crop_box->width;
  create_params.height = crop_box->height;
  create_params.size = 0;
  create_params.isContiguous = 1;
  create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create_params.layout = NVBUF_LAYOUT_PITCH;
#ifdef __aarch64__
  create_params.memType = NVBUF_MEM_DEFAULT;
#else
  // Use CUDA_UNIFIED - supported by NvBufSurfTransform and accessible from CPU
  create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
#endif
  
  GST_DEBUG("Creating destination surface: %ux%u (RGBA, memType=%d)", 
          crop_box->width, crop_box->height, create_params.memType);
  
  if (NvBufSurfaceCreate(&dst_surface, 1, &create_params) != 0) {
    GST_ERROR("Failed to create destination surface");
    return NULL;
  }
  
  GST_DEBUG("Created dst_surface with memType=%d", dst_surface->memType);
  
  // Setup transform parameters for cropping AND color conversion
  NvBufSurfTransformParams transform_params = {0};
  NvBufSurfTransformRect src_rect = {0};
  NvBufSurfTransformRect dst_rect = {0};
  
  src_rect.top = crop_box->top;
  src_rect.left = crop_box->left;
  src_rect.width = crop_box->width;
  src_rect.height = crop_box->height;
  
  dst_rect.top = 0;
  dst_rect.left = 0;
  dst_rect.width = crop_box->width;
  dst_rect.height = crop_box->height;
  
  transform_params.src_rect = &src_rect;
  transform_params.dst_rect = &dst_rect;
  transform_params.transform_flag = NVBUFSURF_TRANSFORM_CROP_SRC |
                                    NVBUFSURF_TRANSFORM_CROP_DST |
                                    NVBUFSURF_TRANSFORM_FILTER;
  transform_params.transform_filter = NvBufSurfTransformInter_Default;
  
  // Perform GPU-accelerated crop and format conversion
  NvBufSurfTransformConfigParams config_params = {0};
  config_params.compute_mode = NvBufSurfTransformCompute_Default;
  config_params.gpu_id = surface->gpuId;
  config_params.cuda_stream = NULL;
  
  GST_DEBUG("Setting transform session params");
  GST_DEBUG("Source format: %d, Dest format: %d", 
          surface->surfaceList[batch_id].colorFormat, 
          dst_surface->surfaceList[0].colorFormat);
  
  if (NvBufSurfTransformSetSessionParams(&config_params) != 0) {
    GST_ERROR("Failed to set transform session params");
    NvBufSurfaceDestroy(dst_surface);
    return NULL;
  }
  
  GST_DEBUG("Performing surface transform (crop + color convert) for batch_id=%u", batch_id);
  
  // NvBufSurfTransform operates on all surfaces in the batch.
  // Temporarily adjust so only the target frame (batch_id) is processed.
  NvBufSurfaceParams orig_first = surface->surfaceList[0];
  guint orig_numFilled = surface->numFilled;
  guint orig_batchSize = surface->batchSize;

  if (batch_id > 0) {
    surface->surfaceList[0] = surface->surfaceList[batch_id];
  }
  surface->numFilled = 1;
  surface->batchSize = 1;


  NvBufSurfTransform_Error transform_err = NvBufSurfTransform(surface, dst_surface, &transform_params);
  if (transform_err != NvBufSurfTransformError_Success) {
    GST_ERROR("Failed to transform surface for batch_id=%u, error=%d", batch_id, transform_err);
    NvBufSurfaceDestroy(dst_surface);
    return NULL;
  }

  // Restore original surface state
  surface->surfaceList[0] = orig_first;
  surface->numFilled = orig_numFilled;
  surface->batchSize = orig_batchSize;

  // Synchronize CUDA operations to ensure transform is complete
  cudaError_t cuda_err = cudaStreamSynchronize(0);
  if (cuda_err != cudaSuccess) {
    GST_WARNING("cudaStreamSynchronize failed: %s", cudaGetErrorString(cuda_err));
  }
  
  NvBufSurfaceParams *dst_params = &dst_surface->surfaceList[0];
  guint crop_w = dst_params->width;
  guint crop_h = dst_params->height;
  guint dst_pitch = dst_params->pitch;
  NvBufSurfaceColorFormat dst_color_format = dst_params->colorFormat;
  
  GST_DEBUG("dst_params: width=%u, height=%u, pitch=%u, colorFormat=%d, memType=%d",
          crop_w, crop_h, dst_pitch, dst_color_format, dst_surface->memType);
  GST_DEBUG("dataPtr=%p", dst_params->dataPtr);
  
  // Allocate CPU buffer for the image data
  guint buffer_size = dst_pitch * crop_h;
  guchar *cpu_buffer = (guchar *)g_malloc(buffer_size);
  if (!cpu_buffer) {
    GST_ERROR("Failed to allocate CPU buffer (%u bytes)", buffer_size);
    NvBufSurfaceDestroy(dst_surface);
    return NULL;
  }
  
  gboolean data_copied = FALSE;
  
  // For CUDA_UNIFIED memory, we can access it directly from CPU after sync
  // But we still need to copy it to our own buffer to be safe
  if (dst_surface->memType == NVBUF_MEM_CUDA_UNIFIED && dst_params->dataPtr) {
    GST_DEBUG("CUDA_UNIFIED memory, copying via cudaMemcpy");
    cuda_err = cudaMemcpy(cpu_buffer, dst_params->dataPtr, buffer_size, cudaMemcpyDeviceToHost);
    if (cuda_err == cudaSuccess) {
      data_copied = TRUE;
      GST_DEBUG("cudaMemcpy succeeded");
    } else {
      GST_WARNING("cudaMemcpy failed: %s, trying direct access", cudaGetErrorString(cuda_err));
      // For unified memory, direct access might work after sync
      cudaDeviceSynchronize();
      memcpy(cpu_buffer, dst_params->dataPtr, buffer_size);
      data_copied = TRUE;
      GST_DEBUG("Direct memcpy from unified memory succeeded");
    }
  }
  
  if (!data_copied) {
    // Try mapping the surface
    GST_DEBUG("Attempting to map surface");
    if (NvBufSurfaceMap(dst_surface, 0, 0, NVBUF_MAP_READ) == 0) {
      NvBufSurfaceSyncForCpu(dst_surface, 0, 0);
      
      guchar *mapped_data = NULL;
      if (dst_params->mappedAddr.addr[0]) {
        mapped_data = (guchar *)dst_params->mappedAddr.addr[0];
      } else if (dst_params->dataPtr) {
        mapped_data = (guchar *)dst_params->dataPtr;
      }
      
      if (mapped_data) {
        GST_DEBUG("Mapped data available at %p", mapped_data);
        memcpy(cpu_buffer, mapped_data, buffer_size);
        data_copied = TRUE;
      }
      
      NvBufSurfaceUnMap(dst_surface, 0, 0);
    } else {
      GST_DEBUG("Map failed");
    }
  }
  
  if (!data_copied && dst_params->dataPtr) {
    // Last resort: try cudaMemcpy even if memType detection failed
    GST_DEBUG("Last resort: cudaMemcpy from %p", dst_params->dataPtr);
    cuda_err = cudaMemcpy(cpu_buffer, dst_params->dataPtr, buffer_size, cudaMemcpyDeviceToHost);
    if (cuda_err == cudaSuccess) {
      data_copied = TRUE;
      GST_DEBUG("cudaMemcpy succeeded");
    } else {
      GST_ERROR("cudaMemcpy failed: %s", cudaGetErrorString(cuda_err));
    }
  }
  
  if (!data_copied) {
    GST_ERROR("Failed to copy surface data to CPU");
    g_free(cpu_buffer);
    NvBufSurfaceDestroy(dst_surface);
    return NULL;
  }
  
  // Debug: Print first 32 bytes of data
  GST_DEBUG("First 32 bytes of CPU data:");
  for (int i = 0; i < 32 && i < (int)buffer_size; i++) {
    GST_DEBUG("%02x ", cpu_buffer[i]);
  }
  GST_DEBUG("\n");
  
  NvBufSurfaceDestroy(dst_surface);
  
  // Now convert from RGBA to RGB for JPEG encoding
  guint rgb_row_bytes = crop_w * 3;
  guchar *rgb_data = (guchar *)g_malloc(rgb_row_bytes * crop_h);
  if (!rgb_data) {
    GST_ERROR("Failed to allocate RGB buffer");
    g_free(cpu_buffer);
    return NULL;
  }
  
  GST_DEBUG("Converting RGBA to RGB: %ux%u", crop_w, crop_h);
  
  // RGBA to RGB conversion
  for (guint y = 0; y < crop_h; y++) {
    guchar *src_row = cpu_buffer + y * dst_pitch;
    guchar *dst_row = rgb_data + y * rgb_row_bytes;
    
    for (guint x = 0; x < crop_w; x++) {
      dst_row[x * 3 + 0] = src_row[x * 4 + 0];  // R
      dst_row[x * 3 + 1] = src_row[x * 4 + 1];  // G
      dst_row[x * 3 + 2] = src_row[x * 4 + 2];  // B
      // Skip alpha (x * 4 + 3)
    }
  }
  
  g_free(cpu_buffer);
  
  GST_DEBUG("Encoding to JPEG");
  
  // Encode to JPEG in memory
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  
  unsigned char *jpeg_buffer = NULL;
  unsigned long jpeg_size = 0;
  
  jpeg_mem_dest(&cinfo, &jpeg_buffer, &jpeg_size);
  
  cinfo.image_width = crop_w;
  cinfo.image_height = crop_h;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  
  jpeg_start_compress(&cinfo, TRUE);
  
  JSAMPROW row_pointer[1];
  while (cinfo.next_scanline < cinfo.image_height) {
    row_pointer[0] = &rgb_data[cinfo.next_scanline * rgb_row_bytes];
    jpeg_write_scanlines(&cinfo, row_pointer, 1);
  }
  
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  
  g_free(rgb_data);
  
  // Encode to base64
  gchar *base64_image = g_base64_encode(jpeg_buffer, jpeg_size);
  
  free(jpeg_buffer); // libjpeg uses malloc
  
  GST_DEBUG("Successfully encoded image to base64 (size=%lu)", jpeg_size);
  
  return base64_image;
}

// =============================================================================
// Frame Saving Functions
// =============================================================================

static gboolean
ensure_frame_save_directory(const gchar *dir_path)
{
  if (!dir_path) {
    return FALSE;
  }
  
  // Check if directory exists
  if (g_file_test(dir_path, G_FILE_TEST_IS_DIR)) {
    return TRUE;
  }
  
  // Try to create directory
  if (g_mkdir_with_parents(dir_path, 0755) != 0) {
    GST_ERROR("Failed to create directory: %s", dir_path);
    return FALSE;
  }
  
  GST_INFO("Created frame save directory: %s", dir_path);
  return TRUE;
}


// Saves a frame as JPEG into:
//   <base_output_dir>/source_<source_id>/frame_....jpg
//
// Returns (on success):
//   "source_<source_id>/frame_....jpg"   <-- RELATIVE PATH
//
// Returns NULL on failure
//
// Caller MUST g_free() the returned string

static gchar *
save_frame_to_jpeg(NvBufSurface *surface,
                   NvDsFrameMeta *frame_meta,
                   const gchar *base_output_dir,
                   gint quality)
{
  /* ----------------------------------------------------
   * Basic validation
   * -------------------------------------------------- */
  if (!surface || !frame_meta || !base_output_dir) {
    GST_ERROR("Invalid parameters for save_frame_to_jpeg");
    return NULL;
  }

  if (surface->numFilled < 1 || !surface->surfaceList) {
    GST_ERROR("Surface has no data");
    return NULL;
  }

  guint batch_id = frame_meta->batch_id;
  NvBufSurfaceParams *src_params = &surface->surfaceList[batch_id];
  if (src_params->width == 0 || src_params->height == 0) {
    GST_ERROR("Invalid surface dimensions");
    return NULL;
  }

  guint source_id   = frame_meta->source_id;
  guint frame_num   = frame_meta->frame_num;
  gboolean infer_ok = frame_meta->bInferDone;

  /* ----------------------------------------------------
   * Build per-source directories
   * -------------------------------------------------- */
  gchar *rel_dir = g_strdup_printf("source_%u", source_id);
  gchar *abs_dir = g_strdup_printf("%s/%s", base_output_dir, rel_dir);

  if (!ensure_frame_save_directory(abs_dir)) {
    GST_ERROR("Failed to create directory: %s", abs_dir);
    g_free(rel_dir);
    g_free(abs_dir);
    return NULL;
  }

  /* ----------------------------------------------------
   * Filename
   * -------------------------------------------------- */
  GDateTime *now = g_date_time_new_now_local();
  gchar *timestamp = g_date_time_format(now, "%Y%m%d_%H%M%S");
  g_date_time_unref(now);

  gchar *filename = g_strdup_printf(
      "frame_src%u_num%u_%d_%s.jpg",
      source_id, frame_num, infer_ok, timestamp);

  g_free(timestamp);

  /* ----------------------------------------------------
   * Relative + Absolute paths
   * -------------------------------------------------- */
  gchar *relative_path = g_strdup_printf("%s/%s", rel_dir, filename);
  gchar *absolute_path = g_strdup_printf("%s/%s", abs_dir, filename);

  g_free(rel_dir);
  g_free(abs_dir);
  g_free(filename);

  GST_DEBUG("Saving frame to: %s", absolute_path);

  /* ----------------------------------------------------
   * Create RGBA destination surface
   * -------------------------------------------------- */
  NvBufSurface *dst_surface = NULL;
  NvBufSurfaceCreateParams create_params = {0};

  create_params.gpuId = surface->gpuId;
  create_params.width = src_params->width;
  create_params.height = src_params->height;
  create_params.isContiguous = 1;
  create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create_params.layout = NVBUF_LAYOUT_PITCH;

#ifdef __aarch64__
  create_params.memType = NVBUF_MEM_DEFAULT;
#else
  create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
#endif

  if (NvBufSurfaceCreate(&dst_surface, 1, &create_params) != 0) {
    GST_ERROR("Failed to create destination surface");
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  /* ----------------------------------------------------
   * GPU color conversion
   * -------------------------------------------------- */
  NvBufSurfTransformParams transform_params = {0};
  transform_params.transform_flag = NVBUFSURF_TRANSFORM_FILTER;
  transform_params.transform_filter = NvBufSurfTransformInter_Default;

  NvBufSurfTransformConfigParams config_params = {0};
  config_params.compute_mode = NvBufSurfTransformCompute_Default;
  config_params.gpu_id = surface->gpuId;
  config_params.cuda_stream = NULL;

  // NvBufSurfTransform operates on all numFilled surfaces in the batch.
  // Temporarily adjust so only the target frame (batch_id) is processed.
  NvBufSurfaceParams orig_first = surface->surfaceList[0];
  guint orig_numFilled = surface->numFilled;
  guint orig_batchSize = surface->batchSize;
  GST_DEBUG("Transforming only batch_id=%u (orig_numFilled=%d, orig_batchSize=%d)", batch_id, orig_numFilled, orig_batchSize);
  if (batch_id > 0) {
    surface->surfaceList[0] = surface->surfaceList[batch_id];
  }
  surface->numFilled = 1;
  surface->batchSize = 1;

  if (NvBufSurfTransformSetSessionParams(&config_params) != 0) {
    GST_ERROR("Failed to set transform session params");
    NvBufSurfaceDestroy(dst_surface);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  NvBufSurfTransform_Error transform_err = NvBufSurfTransform(surface, dst_surface, &transform_params);

  if (transform_err != NvBufSurfTransformError_Success) {
    GST_ERROR("Failed to transform surface for batch_id=%u, error=%d", batch_id, transform_err);
    NvBufSurfaceDestroy(dst_surface);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  // Restore original surface state
  // Restore original surface state
  surface->surfaceList[0] = orig_first;
  surface->numFilled = orig_numFilled;
  surface->batchSize = orig_batchSize;

  /* Ensure GPU work is complete */
  cudaStreamSynchronize(0);

  /* ----------------------------------------------------
   * Copy RGBA data to CPU
   * -------------------------------------------------- */
  NvBufSurfaceParams *dst_params = &dst_surface->surfaceList[0];
  guint width  = dst_params->width;
  guint height = dst_params->height;
  guint pitch  = dst_params->pitch;

  guint buffer_size = pitch * height;
  guchar *cpu_buffer = g_malloc(buffer_size);
  if (!cpu_buffer) {
    GST_ERROR("Failed to allocate CPU buffer");
    NvBufSurfaceDestroy(dst_surface);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  cudaError_t err = cudaMemcpy(cpu_buffer, dst_params->dataPtr, 
                                buffer_size, cudaMemcpyDeviceToHost);
  if (err != cudaSuccess) {
    GST_ERROR("cudaMemcpy failed: %s", cudaGetErrorString(err));
    g_free(cpu_buffer);
    NvBufSurfaceDestroy(dst_surface);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  NvBufSurfaceDestroy(dst_surface);

  /* ----------------------------------------------------
   * RGBA → RGB
   * -------------------------------------------------- */
  guint rgb_stride = width * 3;
  guchar *rgb_data = g_malloc(rgb_stride * height);
  if (!rgb_data) {
    g_free(cpu_buffer);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  for (guint y = 0; y < height; y++) {
    guchar *src = cpu_buffer + y * pitch;
    guchar *dst = rgb_data + y * rgb_stride;
    for (guint x = 0; x < width; x++) {
      dst[x * 3 + 0] = src[x * 4 + 0];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }

  g_free(cpu_buffer);

  /* ----------------------------------------------------
   * JPEG encode
   * -------------------------------------------------- */
  FILE *outfile = fopen(absolute_path, "wb");
  if (!outfile) {
    GST_ERROR("Failed to open file: %s", absolute_path);
    g_free(rgb_data);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, outfile);

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  JSAMPROW row[1];
  while (cinfo.next_scanline < cinfo.image_height) {
    row[0] = &rgb_data[cinfo.next_scanline * rgb_stride];
    jpeg_write_scanlines(&cinfo, row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  fclose(outfile);

  g_free(rgb_data);
  g_free(absolute_path);

  /* ----------------------------------------------------
   * SUCCESS → return RELATIVE path
   * -------------------------------------------------- */
  return relative_path;   // caller must g_free()
}


// =============================================================================
// Landmark Processing Functions
// =============================================================================

static Landmark *
extract_landmarks_from_object(NvDsObjectMeta *obj_meta, guint *num_landmarks_out)
{
  if (obj_meta->mask_params.size == 0) {
    *num_landmarks_out = 0;
    GST_DEBUG("Landmark data size is zero");
    return NULL;
  }

  if(!obj_meta->mask_params.data){
    *num_landmarks_out = 0;
    GST_DEBUG("Landmark data pointer is NULL");
    return NULL;
  }

  guint num_joints = obj_meta->mask_params.size / (sizeof(float) * 3);
  if (num_joints == 0) {
    *num_landmarks_out = 0;
    GST_DEBUG("Landmark data size is zero");
    return NULL;
  }

  gfloat gain = MIN((gfloat)obj_meta->mask_params.width / STREAMMUX_WIDTH,
                    (gfloat)obj_meta->mask_params.height / STREAMMUX_HEIGHT);
  gfloat pad_x = (obj_meta->mask_params.width - STREAMMUX_WIDTH * gain) * 0.5f;
  gfloat pad_y = (obj_meta->mask_params.height - STREAMMUX_HEIGHT * gain) * 0.5f;

  Landmark *landmarks = g_malloc(sizeof(Landmark) * num_joints);

  for (guint i = 0; i < num_joints; i++) {
    landmarks[i].x = (obj_meta->mask_params.data[i * 3 + 0] - pad_x) / gain;
    landmarks[i].y = (obj_meta->mask_params.data[i * 3 + 1] - pad_y) / gain;
    landmarks[i].confidence = obj_meta->mask_params.data[i * 3 + 2];
  }

  *num_landmarks_out = num_joints;
  return landmarks;
}

// =============================================================================
// JSON Building Functions
// =============================================================================

static gchar *
build_detection_json(FaceContext *ctx)
{
  const CropBox* crop_box = ctx->crop_box;
  const CropBox* bbox = ctx->bbox;

  GString *json = g_string_new("{");

  // Core metadata
  
  g_string_append_printf(json, "\"timestamp\":%.3f,", ctx->frame_timestamp);
  g_string_append_printf(json, "\"source_id\":%u,", ctx->source_id);
  g_string_append_printf(json, "\"object_id\":%lu,", ctx->object_id);
  g_string_append_printf(json, "\"class_id\":%d,", ctx->class_id);
  g_string_append_printf(json, "\"confidence\":%.4f,", ctx->confidence);

  // Frame info
  g_string_append_printf(json, "\"frame_size\":{\"width\":%d,\"height\":%d},",
                         STREAMMUX_WIDTH, STREAMMUX_HEIGHT);
  g_string_append_printf(json, "\"frame_number\":%u,", ctx->frame_num);

  // Bounding boxes
  g_string_append_printf(json, "\"bbox\":{\"left\":%u,\"top\":%u,\"width\":%u,\"height\":%u},",
                         bbox->left, bbox->top,
                         bbox->width, bbox->height);
  g_string_append_printf(json, "\"crop_bbox\":{\"left\":%u,\"top\":%u,\"width\":%u,\"height\":%u},",
                         crop_box->left, crop_box->top, crop_box->width, crop_box->height);

  // Landmarks
  g_string_append(json, "\"landmarks\":[");
  for (guint i = 0; i < ctx->num_landmarks; i++) {
    g_string_append_printf(json, "{\"x\":%.2f,\"y\":%.2f,\"confidence\":%.4f}%s",
                           ctx->landmarks[i].x, ctx->landmarks[i].y, 
                           ctx->landmarks[i].confidence,
                           (i < ctx->num_landmarks - 1) ? "," : "");
  }
  g_string_append(json, "],");

  // Face quality metrics
  g_string_append(json, "\"face_quality\":{");
  g_string_append_printf(json, "\"is_good_face\":%s,", ctx->is_good_face ? "true" : "false");
  g_string_append_printf(json, "\"quality_score\":%.3f", ctx->quality_score);
  if (ctx->metrics) {
    g_string_append_printf(json, ",\"is_frontal\":%s", ctx->metrics->is_frontal ? "true" : "false");
    g_string_append_printf(json, ",\"visible_landmarks\":%u", ctx->metrics->visible_landmarks);
    g_string_append_printf(json, ",\"total_landmarks\":%u", ctx->metrics->total_landmarks);
    g_string_append_printf(json, ",\"avg_confidence\":%.3f", ctx->metrics->avg_confidence);
    g_string_append_printf(json, ",\"frontal_score\":%.3f", ctx->metrics->frontal_score);
  }
  g_string_append(json, "}");

  // Face image (optional)
  if (ctx->face_image_base64) {
    g_string_append_printf(json, ",\"face_image\":\"%s\"", ctx->face_image_base64);
  }

  if(ctx->frame_image_path) {
    g_string_append_printf(json, ",\"frame_image_path\":\"%s\"", ctx->frame_image_path);
  }

  g_string_append(json, "}");

  return g_string_free(json, FALSE);
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
  if (KAFKA_ENABLED && detection_manager && detection_manager->enabled) {
      // Build JSON payload
      gchar *json_data = build_detection_json(&ctx);

      // Queue detection for Kafka
      detection_manager_queue(detection_manager, ctx.object_id,
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

gint
main(gint argc, char *argv[])
{
  // Initialize GStreamer and GST debug category before any GST_* logging
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(deepstream_debug_category, "deepstream", 0, "DeepStream Face App");

  GOptionContext *ctx = g_option_context_new("DeepStream");
  GOptionGroup *group = g_option_group_new("deepstream", NULL, NULL, NULL, NULL);
  GError *error = NULL;
  g_option_group_add_entries(group, entries);
  g_option_context_set_main_group(ctx, group);
  g_option_context_add_group(ctx, gst_init_get_option_group());
  if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
    g_option_context_free(ctx);
    g_printerr("ERROR - %s\n", error->message);
    g_error_free(error);
    return -1;
  }
  g_option_context_free(ctx);

  // if (!SOURCE) {
  //   g_printerr("ERROR - Source not found\n");
  //   return -1;
  // }
  NUM_SOURCES = g_strv_length(SOURCES);

  if (NUM_SOURCES == 0) {
    g_printerr("ERROR - No sources provided\n");
    return -1;
  }

  if (STREAMMUX_BATCH_SIZE < NUM_SOURCES) {
    STREAMMUX_BATCH_SIZE = NUM_SOURCES;
    g_print("Setting batch-size to %d to match number of sources\n", STREAMMUX_BATCH_SIZE);
  }


  if (!INFER_CONFIG) {
    g_printerr("ERROR - Config infer not found\n");
    return -1;
  }

  // Initialize frame save directory if enabled
  if (ENABLE_FRAME_SAVE) {
    if (!FRAME_SAVE_DIR) {
      FRAME_SAVE_DIR = g_strdup("./outputs/frames");
    }
    
    if (!ensure_frame_save_directory(FRAME_SAVE_DIR)) {
      g_printerr("ERROR - Failed to create frame save directory: %s\n", FRAME_SAVE_DIR);
      return -1;
    }
    
    GST_INFO("Frame saving enabled: dir=%s, quality=%u", 
            FRAME_SAVE_DIR, FRAME_SAVE_QUALITY);
  }

  gint current_device = -1;
  cudaGetDevice(&current_device);
 
  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, current_device);

  if (prop.integrated) {
    JETSON = TRUE;
  }

  // Check if Kafka is enabled
  if (KAFKA_BROKER) {
    KAFKA_ENABLED = TRUE;
    if (!KAFKA_TOPIC) {
      KAFKA_TOPIC = g_strdup("face-detections");
    }
  }

  // Initialize detection manager
  init_detection_manager();

  GMainLoop *loop = g_main_loop_new(NULL, FALSE);

  _intr_setup();
  g_timeout_add(400, check_for_interrupt, &loop);

  // Start periodic check for pending detections
  if (KAFKA_ENABLED && detection_manager) {
    g_timeout_add(500, detection_manager_process_pending_callback, NULL);
  }

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

  GstElement *nvinfer = gst_element_factory_make("nvinfer", "nvinfer");
  if (!nvinfer || !gst_bin_add(GST_BIN(pipeline), nvinfer)) {
    g_printerr("ERROR - Failed to create nvinfer\n");
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

  //================================================
  GST_DEBUG("\n");
  //GST_DEBUG("SOURCE: %s", SOURCE);
  GST_DEBUG("NUM_SOURCES: %d", NUM_SOURCES);
  GST_DEBUG("INFER_CONFIG: %s", INFER_CONFIG);
  GST_DEBUG("STREAMMUX_BATCH_SIZE: %d", STREAMMUX_BATCH_SIZE);
  GST_DEBUG("STREAMMUX_WIDTH: %d", STREAMMUX_WIDTH);
  GST_DEBUG("STREAMMUX_HEIGHT: %d", STREAMMUX_HEIGHT);
  GST_DEBUG("GPU_ID: %d", GPU_ID);
  GST_DEBUG("PERF_MEASUREMENT_INTERVAL_SEC: %d", PERF_MEASUREMENT_INTERVAL_SEC);
  GST_DEBUG("JETSON: %s", JETSON ? "TRUE" : "FALSE");
  if (KAFKA_ENABLED) {
    GST_DEBUG("KAFKA_BROKER: %s", KAFKA_BROKER);
    GST_DEBUG("KAFKA_TOPIC: %s", KAFKA_TOPIC);
    GST_DEBUG("KAFKA_SEND_DELAY_SEC: %.1f", KAFKA_SEND_DELAY_SEC);
    GST_DEBUG("KAFKA_QUALITY_IMPROVEMENT_THRESHOLD: %.2f", KAFKA_QUALITY_IMPROVEMENT_THRESHOLD);
  }
  GST_DEBUG("ENABLE_CROP_IMAGE: %s", ENABLE_CROP_IMAGE ? "TRUE" : "FALSE");
  if (ENABLE_FRAME_SAVE) {
    GST_DEBUG("FRAME_SAVE_DIR: %s", FRAME_SAVE_DIR);
    GST_DEBUG("FRAME_SAVE_QUALITY: %u", FRAME_SAVE_QUALITY);
  }
  GST_DEBUG("\n");

  // wait user to press enter key to start
  if (WAIT_FOR_USER_INPUT) {
    g_print("Press ENTER to start processing ...\n");
    getchar();
  }

  GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=RGBA");
  g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
  gst_caps_unref(caps);

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
    g_object_set(G_OBJECT(nvinfer), "gpu_id", GPU_ID, NULL);
    g_object_set(G_OBJECT(nvvidconv), "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE, "gpu_id", GPU_ID, NULL);
  }

  //==============================================
  // Link the elements together
  if (!gst_element_link_many(nvstreammux, nvinfer, nvtracker, nvvidconv, 
                           capsfilter, tee, NULL)) {
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
  gst_element_set_state(pipeline, GST_STATE_PAUSED);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("ERROR - Failed to set pipeline to playing\n");
    return -1;
  }

  GST_DEBUG("\n");

  g_main_loop_run(loop);

  gst_element_set_state(pipeline, GST_STATE_NULL);

  // Cleanup detection manager
  cleanup_detection_manager();

  g_free(perf_struct);

  // if (SOURCE) {
  //   g_free(SOURCE);
  // }
  if (SOURCES) {
    g_strfreev(SOURCES);  // Frees array and all strings
  }


  if (INFER_CONFIG) {
    g_free(INFER_CONFIG);
  }

  if (KAFKA_BROKER) {
    g_free(KAFKA_BROKER);
  }

  if (KAFKA_TOPIC) {
    g_free(KAFKA_TOPIC);
  }

  if (FRAME_SAVE_DIR) {
    g_free(FRAME_SAVE_DIR);
  }

  gst_object_unref(GST_OBJECT(pipeline));
  g_source_remove(bus_watch_id);
  g_main_loop_unref(loop);

  return 0;
}
