#include "deepstream.h"

GOptionEntry entries[] = {
  {"source", 's', 0, G_OPTION_ARG_STRING, &SOURCE, "Source stream/file", NULL},
  {"infer-config", 'c', 0, G_OPTION_ARG_STRING, &INFER_CONFIG, "Config infer file", NULL},
  {"streammux-batch-size", 'b', 0, G_OPTION_ARG_INT, &STREAMMUX_BATCH_SIZE, "Streammux batch-size (default 1)", NULL},
  {"streammux-width", 'w', 0, G_OPTION_ARG_INT, &STREAMMUX_WIDTH, "Streammux width (default 1920)", NULL},
  {"streammux-height", 'e', 0, G_OPTION_ARG_INT, &STREAMMUX_HEIGHT, "Streammux height (default 1080)", NULL},
  {"gpu-id", 'g', 0, G_OPTION_ARG_INT, &GPU_ID, "GPU id (default 0)", NULL},
  {"kafka-broker", 'k', 0, G_OPTION_ARG_STRING, &KAFKA_BROKER, "Kafka broker address (e.g., localhost:9092)", NULL},
  {"kafka-topic", 't', 0, G_OPTION_ARG_STRING, &KAFKA_TOPIC, "Kafka topic name (default: face-detections)", NULL},
  {"kafka-delay", 'd', 0, G_OPTION_ARG_DOUBLE, &KAFKA_SEND_DELAY_SEC, "Delay in seconds before sending to Kafka (default: 2.0)", NULL},
  {"kafka-quality-threshold", 'q', 0, G_OPTION_ARG_DOUBLE, &KAFKA_QUALITY_IMPROVEMENT_THRESHOLD, "Minimum quality improvement to resend (default: 0.1)", NULL},
  {NULL}
};

static int MAX_DISPLAY_LEN = 128;

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
    g_print("INFO - Kafka not connected, would send: object_id=%lu, quality=%.3f\n",
            detection->object_id, detection->quality_score);
    return TRUE;
  }
  
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
    g_printerr("ERROR - Failed to produce message: %s\n", 
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
      g_print("INFO - Cleanup: removed %u sent records, %u pending detections\n",
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
      g_printerr("WARNING - Failed to create Kafka topic, running without Kafka\n");
      kafka_producer_destroy(detection_manager->kafka_producer, NULL);
      detection_manager->kafka_producer = NULL;
    }
  } else {
    g_printerr("WARNING - Failed to create Kafka producer, running without Kafka\n");
  }
#else
  g_print("WARNING - Kafka support not compiled in. Build with KAFKA=1 to enable.\n");
  detection_manager->kafka_producer = NULL;
  detection_manager->kafka_topic = NULL;
#endif
  
  g_print("INFO - Detection manager initialized (Kafka: %s, Topic: %s)\n",
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
// Send Detection to Kafka
// =============================================================================

static void
send_detection_to_kafka(NvDsFrameMeta *frame_meta, NvDsObjectMeta *obj_meta, 
                        Landmark *landmarks, guint num_landmarks,
                        gboolean is_good_face, gdouble quality_score,
                        FaceQualityMetrics *metrics)
{
  if (!detection_manager || !detection_manager->enabled) {
    return;
  }
  
  // Build JSON string
  GString *json = g_string_new("{");
  
  // Timestamp
  g_string_append_printf(json, "\"timestamp\": %.3f,", get_current_time());
  
  // Object info
  g_string_append_printf(json, "\"object_id\": %lu,", obj_meta->object_id);
  g_string_append_printf(json, "\"class_id\": %d,", obj_meta->class_id);
  g_string_append_printf(json, "\"confidence\": %.4f,", obj_meta->confidence);
  
  // Bounding box
  g_string_append_printf(json, "\"bbox\": {\"left\": %.2f, \"top\": %.2f, \"width\": %.2f, \"height\": %.2f},",
                         obj_meta->rect_params.left, obj_meta->rect_params.top,
                         obj_meta->rect_params.width, obj_meta->rect_params.height);
  
  // Landmarks
  g_string_append(json, "\"landmarks\": [");
  for (guint i = 0; i < num_landmarks; i++) {
    g_string_append_printf(json, "{\"x\": %.2f, \"y\": %.2f, \"confidence\": %.4f}%s",
                           landmarks[i].x, landmarks[i].y, landmarks[i].confidence,
                           (i < num_landmarks - 1) ? "," : "");
  }
  g_string_append(json, "],");
  
  // Face quality
  g_string_append_printf(json, "\"face_quality\": {");
  g_string_append_printf(json, "\"is_good_face\": %s,", is_good_face ? "true" : "false");
  g_string_append_printf(json, "\"quality_score\": %.3f,", quality_score);
  if (metrics) {
    g_string_append_printf(json, "\"is_frontal\": %s,", metrics->is_frontal ? "true" : "false");
    g_string_append_printf(json, "\"visible_landmarks\": %u,", metrics->visible_landmarks);
    g_string_append_printf(json, "\"total_landmarks\": %u,", metrics->total_landmarks);
    g_string_append_printf(json, "\"avg_confidence\": %.3f,", metrics->avg_confidence);
    g_string_append_printf(json, "\"frontal_score\": %.3f", metrics->frontal_score);
  }
  g_string_append(json, "},");
  
  // Frame info
  g_string_append_printf(json, "\"frame_number\": %u,", frame_meta->frame_num);
  g_string_append_printf(json, "\"source_id\": %u", frame_meta->source_id);
  
  g_string_append(json, "}");
  
  // Queue detection
  detection_manager_queue(detection_manager, obj_meta->object_id, quality_score, json->str);
  
  g_string_free(json, TRUE);
}

static void
set_custom_bbox(NvDsObjectMeta *obj_meta)
{
  guint border_width = 6;
  guint font_size = 18;

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

static void
parse_face_from_meta(NvDsBatchMeta *batch_meta, NvDsFrameMeta *frame_meta, NvDsObjectMeta *obj_meta)
{
  NvDsDisplayMeta *display_meta = NULL;

  guint num_joints = obj_meta->mask_params.size / (sizeof(float) * 3);

  gfloat gain = MIN((gfloat) obj_meta->mask_params.width / STREAMMUX_WIDTH, (gfloat) obj_meta->mask_params.height /
      STREAMMUX_HEIGHT);

  gfloat pad_x = (obj_meta->mask_params.width - STREAMMUX_WIDTH * gain) * 0.5f;
  gfloat pad_y = (obj_meta->mask_params.height - STREAMMUX_HEIGHT * gain) * 0.5f;

  // Extract landmarks for quality assessment
  Landmark *landmarks = NULL;
  if (num_joints > 0) {
    landmarks = g_malloc(sizeof(Landmark) * num_joints);
  }

  for (guint i = 0; i < num_joints; ++i) {
    gfloat xc = (obj_meta->mask_params.data[i * 3 + 0] - pad_x) / gain;
    gfloat yc = (obj_meta->mask_params.data[i * 3 + 1] - pad_y) / gain;
    gfloat confidence = obj_meta->mask_params.data[i * 3 + 2];

    // Store landmark for quality assessment
    if (landmarks) {
      landmarks[i].x = xc;
      landmarks[i].y = yc;
      landmarks[i].confidence = confidence;
    }

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

  // Assess face quality and send to Kafka if enabled
  if (KAFKA_ENABLED && landmarks && num_joints >= 5) {
    gboolean is_good_face = FALSE;
    gdouble quality_score = 0.0;
    FaceQualityMetrics metrics = {0};
    
    assess_face_quality(landmarks, num_joints, &is_good_face, &quality_score, &metrics);
    
    // Send detection to Kafka
    send_detection_to_kafka(frame_meta, obj_meta, landmarks, num_joints,
                            is_good_face, quality_score, &metrics);
  }

  if (landmarks) {
    g_free(landmarks);
  }

  g_free(obj_meta->mask_params.data);
  obj_meta->mask_params.width = 0;
  obj_meta->mask_params.height = 0;
  obj_meta->mask_params.size = 0;
}

static GstPadProbeReturn
nvosd_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  GstBuffer *buf = (GstBuffer *) info->data;
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);

  NvDsMetaList *l_frame = NULL;
  for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);

    NvDsMetaList *l_obj = NULL;
    for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) (l_obj->data);

      parse_face_from_meta(batch_meta, frame_meta, obj_meta);
      set_custom_bbox(obj_meta);
    }
  }

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
        g_printerr("ERROR - Failed to link source to nvstreammux sink pad\n");
      }
    }
    else {
      g_printerr("ERROR - decodebin did not pick NVIDIA decoder plugin\n");
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
    g_printerr("ERROR - Failed to get nvstreammux %s pad\n", pad_name);
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
      g_print("DEBUG - EOS\n");
      g_main_loop_quit(loop);
      break;
    }
    case GST_MESSAGE_WARNING:
    {
      gchar *debug;
      GError *error;
      gst_message_parse_warning(message, &error, &debug);
      g_printerr("WARNING - %s - %s\n", error->message, debug);
      g_free(debug);
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_ERROR:
    {
      gchar *debug;
      GError *error;
      gst_message_parse_error(message, &error, &debug);
      g_printerr("ERROR - %s - %s\n", error->message, debug);
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

gint
main(gint argc, char *argv[])
{
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

  if (!SOURCE) {
    g_printerr("ERROR - Source not found\n");
    return -1;
  }

  if (!INFER_CONFIG) {
    g_printerr("ERROR - Config infer not found\n");
    return -1;
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

  GstElement *uridecodebin = create_uridecodebin(0, SOURCE, nvstreammux);
  if (!uridecodebin || !gst_bin_add(GST_BIN(pipeline), uridecodebin)) {
    g_printerr("ERROR - Failed to create uridecodebin\n");
    return -1;
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

  GstElement *nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvideoconvert");
  if (!nvvidconv || !gst_bin_add(GST_BIN(pipeline), nvvidconv)) {
    g_printerr("ERROR - Failed to create nvvideoconvert\n");
    return -1;
  }

  GstElement *capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
  if (!capsfilter || !gst_bin_add(GST_BIN(pipeline), capsfilter)) {
    g_printerr("ERROR - Failed to create capsfilter\n");
    return -1;
  }

  GstElement *nvosd = gst_element_factory_make("nvdsosd", "nvdsosd");
  if (!nvosd || !gst_bin_add(GST_BIN(pipeline), nvosd)) {
    g_printerr("ERROR - Failed to create nvdsosd\n");
    return -1;
  }

  GstElement *nvsink = NULL;
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

  g_print("\n");
  g_print("SOURCE: %s\n", SOURCE);
  g_print("INFER_CONFIG: %s\n", INFER_CONFIG);
  g_print("STREAMMUX_BATCH_SIZE: %d\n", STREAMMUX_BATCH_SIZE);
  g_print("STREAMMUX_WIDTH: %d\n", STREAMMUX_WIDTH);
  g_print("STREAMMUX_HEIGHT: %d\n", STREAMMUX_HEIGHT);
  g_print("GPU_ID: %d\n", GPU_ID);
  g_print("PERF_MEASUREMENT_INTERVAL_SEC: %d\n", PERF_MEASUREMENT_INTERVAL_SEC);
  g_print("JETSON: %s\n", JETSON ? "TRUE" : "FALSE");
  if (KAFKA_ENABLED) {
    g_print("KAFKA_BROKER: %s\n", KAFKA_BROKER);
    g_print("KAFKA_TOPIC: %s\n", KAFKA_TOPIC);
    g_print("KAFKA_SEND_DELAY_SEC: %.1f\n", KAFKA_SEND_DELAY_SEC);
    g_print("KAFKA_QUALITY_IMPROVEMENT_THRESHOLD: %.2f\n", KAFKA_QUALITY_IMPROVEMENT_THRESHOLD);
  }
  g_print("\n");

  GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=RGBA");
  g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
  gst_caps_unref(caps);

  g_object_set(G_OBJECT(nvstreammux), "batch-size", STREAMMUX_BATCH_SIZE, "batched-push-timeout", 25000,
      "width", STREAMMUX_WIDTH, "height", STREAMMUX_HEIGHT, "live-source", 1, NULL);
  g_object_set(G_OBJECT(nvinfer), "config-file-path", INFER_CONFIG, "qos", 0, NULL);
  g_object_set(G_OBJECT(nvtracker), "tracker-width", 640, "tracker-height", 384,
      "ll-lib-file", "/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so",
      "ll-config-file", "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml",
      "gpu-id", GPU_ID, "display-tracking-id", 1, NULL);
  g_object_set(G_OBJECT(nvosd), "process-mode", MODE_GPU, "qos", 0, NULL);
  g_object_set(G_OBJECT(nvsink), "async", 0, "sync", 0, "qos", 0, NULL);

  // set nvsink window-width and height
  g_object_set(G_OBJECT(nvsink), "window-width", 400, "window-height", 400, NULL);

  if (g_strrstr(SOURCE, "file://")) {
    g_object_set(G_OBJECT(nvstreammux), "live-source", 0, NULL);
  }

  if (!JETSON) {
    g_object_set(G_OBJECT(nvstreammux), "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE, "gpu_id", GPU_ID, NULL);
    g_object_set(G_OBJECT(nvinfer), "gpu_id", GPU_ID, NULL);
    g_object_set(G_OBJECT(nvvidconv), "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE, "gpu_id", GPU_ID, NULL);
    g_object_set(G_OBJECT(nvosd), "gpu_id", GPU_ID, NULL);
  }

  if (!gst_element_link_many(nvstreammux, nvinfer, nvtracker, nvvidconv, capsfilter, nvosd, nvsink, NULL)) {
    g_printerr("ERROR - Failed to link pipeline elements\n");
    return -1;
  }

  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  guint bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
  gst_object_unref(bus);

  GstPad *nvosd_sink_pad = gst_element_get_static_pad(nvosd, "sink");
  if (!nvosd_sink_pad) {
    g_printerr("ERROR - Failed to get nvosd sink pad\n");
    return -1;
  }

  gst_pad_add_probe(nvosd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, nvosd_sink_pad_buffer_probe, NULL, NULL);

  NvDsAppPerfStructInt *perf_struct = (NvDsAppPerfStructInt *) g_malloc0(sizeof(NvDsAppPerfStructInt));
  enable_perf_measurement(perf_struct, nvosd_sink_pad, 1, PERF_MEASUREMENT_INTERVAL_SEC, 0, perf_cb);

  gst_object_unref(nvosd_sink_pad);

  gst_element_set_state(pipeline, GST_STATE_PAUSED);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("ERROR - Failed to set pipeline to playing\n");
    return -1;
  }

  g_print("\n");

  g_main_loop_run(loop);

  gst_element_set_state(pipeline, GST_STATE_NULL);

  // Cleanup detection manager
  cleanup_detection_manager();

  g_free(perf_struct);

  if (SOURCE) {
    g_free(SOURCE);
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

  gst_object_unref(GST_OBJECT(pipeline));
  g_source_remove(bus_watch_id);
  g_main_loop_unref(loop);

  g_print("\n");

  return 0;
}
