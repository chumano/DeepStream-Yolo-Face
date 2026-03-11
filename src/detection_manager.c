#include "detection_manager.h"
#include <gst/gst.h>
#include <time.h>

GST_DEBUG_CATEGORY_STATIC(detection_manager_debug_category);
#define GST_CAT_DEFAULT detection_manager_debug_category

// =============================================================================
// Internal Structures
// =============================================================================

typedef struct _DetectionKey {
  guint source_id;
  guint64 object_id;
} DetectionKey;

typedef struct _Detection {
  guint source_id;
  guint64 object_id;
  gdouble quality_score;
  gdouble timestamp;
  gboolean is_resend;
  gchar *json_data;
} Detection;

typedef struct _DetectionRecord {
  guint source_id;
  guint64 object_id;
  gdouble quality_score;
  gdouble sent_timestamp;
  guint send_count;
  gdouble last_seen_timestamp;
} DetectionRecord;

typedef struct _DetectionStore {
  GHashTable *pending;
  GHashTable *sent;
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

/** Lightweight wrapper for a one-shot Kafka event (not tied to a tracked object). */
typedef struct {
  const gchar *event_type;  /**< placed in the "event_type" Kafka message header */
  const gchar *json_data;   /**< message payload (borrowed, not freed here) */
} KafkaEvent;

struct _DetectionManager {
  gboolean enabled;
  gchar *broker;
  gchar *topic;        /**< Kafka topic for face-detection messages */
  gchar *event_topic;  /**< Kafka topic for one-shot events (NULL → falls back to topic) */
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
  void *kafka_producer;
  void *kafka_topic;
#endif
};

// =============================================================================
// Utility Functions
// =============================================================================

static gdouble get_current_time(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (gdouble)ts.tv_sec + (gdouble)ts.tv_nsec / 1000000000.0;
}

// =============================================================================
// Detection Store Functions
// =============================================================================

static guint detection_key_hash(gconstpointer key)
{
  const DetectionKey *dk = (const DetectionKey *)key;
  return g_int_hash(&dk->source_id) ^ g_int64_hash(&dk->object_id);
}

static gboolean detection_key_equal(gconstpointer a, gconstpointer b)
{
  const DetectionKey *ka = (const DetectionKey *)a;
  const DetectionKey *kb = (const DetectionKey *)b;
  return (ka->source_id == kb->source_id) && (ka->object_id == kb->object_id);
}

static DetectionKey* detection_key_new(guint source_id, guint64 object_id)
{
  DetectionKey *key = g_malloc(sizeof(DetectionKey));
  key->source_id = source_id;
  key->object_id = object_id;
  return key;
}

static void detection_free(Detection *detection)
{
  if (detection) {
    if (detection->json_data) {
      g_free(detection->json_data);
    }
    g_free(detection);
  }
}

static void detection_record_free(DetectionRecord *record)
{
  if (record) {
    g_free(record);
  }
}

static DetectionStore* detection_store_new(gdouble sent_record_ttl_sec, gdouble pending_ttl_sec)
{
  DetectionStore *store = g_malloc0(sizeof(DetectionStore));
  store->pending = g_hash_table_new_full(detection_key_hash, detection_key_equal,
                                         g_free, (GDestroyNotify)detection_free);
  store->sent = g_hash_table_new_full(detection_key_hash, detection_key_equal,
                                      g_free, (GDestroyNotify)detection_record_free);
  pthread_mutex_init(&store->pending_lock, NULL);
  pthread_mutex_init(&store->sent_lock, NULL);
  store->sent_record_ttl_sec = sent_record_ttl_sec;
  store->pending_ttl_sec = pending_ttl_sec;
  return store;
}

static void detection_store_free(DetectionStore *store)
{
  if (store) {
    pthread_mutex_destroy(&store->pending_lock);
    pthread_mutex_destroy(&store->sent_lock);
    g_hash_table_destroy(store->pending);
    g_hash_table_destroy(store->sent);
    g_free(store);
  }
}

static Detection* detection_store_get_pending(DetectionStore *store, guint source_id, guint64 object_id)
{
  Detection *result = NULL;
  pthread_mutex_lock(&store->pending_lock);
  DetectionKey key = {.source_id = source_id, .object_id = object_id};
  result = g_hash_table_lookup(store->pending, &key);
  pthread_mutex_unlock(&store->pending_lock);
  return result;
}

static void detection_store_set_pending(DetectionStore *store, Detection *detection)
{
  pthread_mutex_lock(&store->pending_lock);
  DetectionKey *key = detection_key_new(detection->source_id, detection->object_id);
  g_hash_table_replace(store->pending, key, detection);
  pthread_mutex_unlock(&store->pending_lock);
}

static Detection* detection_store_remove_pending(DetectionStore *store, guint source_id, guint64 object_id)
{
  Detection *result = NULL;
  pthread_mutex_lock(&store->pending_lock);
  DetectionKey key = {.source_id = source_id, .object_id = object_id};
  result = g_hash_table_lookup(store->pending, &key);
  if (result) {
    g_hash_table_steal(store->pending, &key);
  }
  pthread_mutex_unlock(&store->pending_lock);
  return result;
}

static DetectionRecord* detection_store_get_sent(DetectionStore *store, guint source_id, guint64 object_id)
{
  DetectionRecord *result = NULL;
  pthread_mutex_lock(&store->sent_lock);
  DetectionKey key = {.source_id = source_id, .object_id = object_id};
  result = g_hash_table_lookup(store->sent, &key);
  pthread_mutex_unlock(&store->sent_lock);
  return result;
}

static void detection_store_record_sent(DetectionStore *store, Detection *detection)
{
  pthread_mutex_lock(&store->sent_lock);
  
  DetectionKey key = {.source_id = detection->source_id, .object_id = detection->object_id};
  DetectionRecord *existing = g_hash_table_lookup(store->sent, &key);
  if (existing) {
    existing->quality_score = detection->quality_score;
    existing->sent_timestamp = get_current_time();
    existing->send_count++;
    existing->last_seen_timestamp = existing->sent_timestamp;
  } else {
    DetectionKey *new_key = detection_key_new(detection->source_id, detection->object_id);
    DetectionRecord *record = g_malloc0(sizeof(DetectionRecord));
    record->source_id = detection->source_id;
    record->object_id = detection->object_id;
    record->quality_score = detection->quality_score;
    record->sent_timestamp = get_current_time();
    record->send_count = 1;
    record->last_seen_timestamp = record->sent_timestamp;
    g_hash_table_replace(store->sent, new_key, record);
  }
  
  pthread_mutex_unlock(&store->sent_lock);
}

static void detection_store_update_last_seen(DetectionStore *store, guint source_id, guint64 object_id)
{
  pthread_mutex_lock(&store->sent_lock);
  DetectionKey key = {.source_id = source_id, .object_id = object_id};
  DetectionRecord *record = g_hash_table_lookup(store->sent, &key);
  if (record) {
    record->last_seen_timestamp = get_current_time();
  }
  pthread_mutex_unlock(&store->sent_lock);
}

static void detection_store_cleanup_stale_records(DetectionStore *store, guint *removed_sent, guint *removed_pending)
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
    DetectionRecord *record = (DetectionRecord *)value;
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
  
  pthread_mutex_lock(&store->pending_lock);
  keys_to_remove = NULL;
  
  g_hash_table_iter_init(&iter, store->pending);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    Detection *detection = (Detection *)value;
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
// Kafka Functions
// =============================================================================

#ifdef KAFKA_ENABLED_BUILD
static void kafka_delivery_report_cb(rd_kafka_t *rk, const rd_kafka_message_t *rkmessage, void *opaque)
{
  if (rkmessage->err) {
    g_printerr("ERROR - Kafka delivery failed: %s\n", rd_kafka_err2str(rkmessage->err));
  }
}

static void kafka_error_cb(rd_kafka_t *rk, int err, const char *reason, void *opaque)
{
  g_printerr("ERROR - Kafka error: %s: %s\n", rd_kafka_err2str(err), reason);
}

static rd_kafka_t* kafka_producer_create(const gchar *broker)
{
  rd_kafka_conf_t *conf = rd_kafka_conf_new();
  char errstr[512];
  
  if (rd_kafka_conf_set(conf, "bootstrap.servers", broker, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    g_printerr("ERROR - Kafka config failed: %s\n", errstr);
    rd_kafka_conf_destroy(conf);
    return NULL;
  }
  
  rd_kafka_conf_set_dr_msg_cb(conf, kafka_delivery_report_cb);
  rd_kafka_conf_set_error_cb(conf, kafka_error_cb);
  
  rd_kafka_conf_set(conf, "queue.buffering.max.messages", "100000", NULL, 0);
  rd_kafka_conf_set(conf, "queue.buffering.max.ms", "100", NULL, 0);
  rd_kafka_conf_set(conf, "batch.num.messages", "1000", NULL, 0);
  
  rd_kafka_t *producer = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
  if (!producer) {
    g_printerr("ERROR - Failed to create Kafka producer: %s\n", errstr);
    return NULL;
  }
  
  g_print("INFO - Kafka producer created for broker: %s\n", broker);
  return producer;
}

static rd_kafka_topic_t* kafka_topic_create(rd_kafka_t *producer, const gchar *topic_name)
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

static void kafka_producer_destroy(rd_kafka_t *producer, rd_kafka_topic_t *topic)
{
  if (topic) {
    rd_kafka_topic_destroy(topic);
  }
  
  if (producer) {
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

/**
 * Send a one-shot KafkaEvent, attaching its event_type as a message header.
 * rd_kafka_produceva takes ownership of the headers on success; the caller
 * must destroy them only on failure.
 */
static gboolean kafka_send_event(DetectionManager *manager, const KafkaEvent *event)
{
  if (manager->kafka_producer == NULL) {
    GST_DEBUG("[send-event] Kafka not connected, dropping event type=%s", event->event_type);
    return TRUE;
  }

  const gchar *topic = manager->event_topic ? manager->event_topic : manager->topic;
  if (!topic) {
    GST_DEBUG("[send-event] No event topic configured, dropping event type=%s", event->event_type);
    return TRUE;
  }

  rd_kafka_headers_t *hdrs = rd_kafka_headers_new(1);
  rd_kafka_header_add(hdrs, "event_type", -1, event->event_type, -1);

  rd_kafka_resp_err_t err = rd_kafka_producev(
      manager->kafka_producer,
      RD_KAFKA_V_TOPIC(topic),
      RD_KAFKA_V_PARTITION(RD_KAFKA_PARTITION_UA),
      RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
      RD_KAFKA_V_VALUE((void *)event->json_data, strlen(event->json_data)),
      RD_KAFKA_V_HEADERS(hdrs),
      RD_KAFKA_V_END
  );

  if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
    GST_ERROR("[send-event] rd_kafka_produceva failed (event_type=%s): %s",
              event->event_type, rd_kafka_err2str(err));
    rd_kafka_headers_destroy(hdrs);  /* librdkafka only takes ownership on success */
    return FALSE;
  }

  rd_kafka_poll(manager->kafka_producer, 0);
  return TRUE;
}
#endif

static gboolean detection_manager_send_kafka(DetectionManager *manager, Detection *detection)
{
#ifdef KAFKA_ENABLED_BUILD
  if (manager->kafka_producer == NULL || manager->kafka_topic == NULL) {
    GST_DEBUG("INFO - Kafka not connected, would send: object_id=%lu, quality=%.3f\n",
            detection->object_id, detection->quality_score);
    return TRUE;
  }
  
  GST_INFO("INFO - Sending to Kafka: object_id=%lu, quality=%.3f, timestamp=%.3f\n",
          detection->object_id, detection->quality_score, detection->timestamp);

  gint err = rd_kafka_produce(
      manager->kafka_topic,
      RD_KAFKA_PARTITION_UA,
      RD_KAFKA_MSG_F_COPY,
      detection->json_data,
      strlen(detection->json_data),
      NULL, 0,
      NULL
  );
  
  if (err == -1) {
    GST_ERROR("ERROR - Failed to produce message: %s\n",
               rd_kafka_err2str(rd_kafka_last_error()));
    return FALSE;
  }
  
  rd_kafka_poll(manager->kafka_producer, 0);
  
  return TRUE;
#else
  if (manager->kafka_producer == NULL) {
    g_print("INFO - Kafka not compiled in, would send: object_id=%lu, quality=%.3f\n",
            detection->object_id, detection->quality_score);
    return TRUE;
  }
  return TRUE;
#endif
}

// =============================================================================
// Internal Manager Functions
// =============================================================================

static void detection_manager_increment_stat(DetectionManager *manager, const gchar *stat)
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

static gboolean detection_manager_should_queue(DetectionManager *manager, Detection *detection,
                                                Detection *pending, DetectionRecord *sent_record)
{
  if (sent_record != NULL) {
    gdouble improvement = detection->quality_score - sent_record->quality_score;
    if (improvement < manager->quality_improvement_threshold) {
      return FALSE;
    }
  }
  
  if (pending != NULL) {
    if (detection->quality_score <= pending->quality_score) {
      return FALSE;
    }
  }
  
  return TRUE;
}

static gboolean detection_manager_should_send(DetectionManager *manager, Detection *detection, gdouble current_time)
{
  gdouble elapsed = current_time - detection->timestamp;
  return (elapsed >= manager->delay_sec);
}

// =============================================================================
// Public API Implementation
// =============================================================================
static gboolean debug_initialized = FALSE;
DetectionManager* detection_manager_new(gboolean enabled,
                                        const gchar *broker,
                                        const gchar *topic,
                                        const gchar *event_topic,
                                        gdouble delay_sec,
                                        gdouble quality_threshold,
                                        gdouble cleanup_interval_sec,
                                        gdouble sent_record_ttl_sec,
                                        gdouble pending_ttl_sec)
{
 if (!debug_initialized) {
    GST_DEBUG_CATEGORY_INIT(detection_manager_debug_category, "detection-manager", 0, 
                            "DeepStream Detection Manager");
    debug_initialized = TRUE;
 }
  DetectionManager *manager = g_malloc0(sizeof(DetectionManager));
  manager->enabled = enabled;
  manager->broker = broker ? g_strdup(broker) : NULL;
  manager->topic = topic ? g_strdup(topic) : NULL;
  manager->event_topic = event_topic ? g_strdup(event_topic) : NULL;
  manager->delay_sec = delay_sec;
  manager->quality_improvement_threshold = quality_threshold;
  manager->cleanup_interval_sec = cleanup_interval_sec;
  manager->last_cleanup_time = get_current_time();
  manager->store = detection_store_new(sent_record_ttl_sec, pending_ttl_sec);
  pthread_mutex_init(&manager->stats.lock, NULL);
  manager->kafka_producer = NULL;
  manager->kafka_topic = NULL;
  return manager;
}

void detection_manager_free(DetectionManager *manager)
{
  if (manager) {
    if (manager->broker) {
      g_free(manager->broker);
    }
    if (manager->topic) {
      g_free(manager->topic);
    }
    if (manager->event_topic) {
      g_free(manager->event_topic);
    }
    if (manager->store) {
      detection_store_free(manager->store);
    }
    pthread_mutex_destroy(&manager->stats.lock);
    g_free(manager);
  }
}

void detection_manager_queue(DetectionManager *manager,
                             guint source_id,
                             guint64 object_id,
                             gdouble quality_score,
                             const gchar *json_data)
{
  if (!manager->enabled) {
    return;
  }
  
  gdouble current_time = get_current_time();
  DetectionRecord *sent_record = detection_store_get_sent(manager->store, source_id, object_id);
  gboolean is_resend = (sent_record != NULL);
  
  if (sent_record != NULL) {
    detection_store_update_last_seen(manager->store, source_id, object_id);
  }
  
  Detection *detection = g_malloc0(sizeof(Detection));
  detection->source_id = source_id;
  detection->object_id = object_id;
  detection->quality_score = quality_score;
  detection->timestamp = current_time;
  detection->is_resend = is_resend;
  detection->json_data = g_strdup(json_data);
  
  Detection *pending = detection_store_get_pending(manager->store, source_id, object_id);
  
  if (!detection_manager_should_queue(manager, detection, pending, sent_record)) {
    detection_manager_increment_stat(manager, "skipped");
    detection_free(detection);
    return;
  }

  if (pending) {
    GST_DEBUG("Replacing pending detection for source=%u object_id=%lu (old_quality=%.3f, new_quality=%.3f)\n",
             source_id, object_id, pending->quality_score, quality_score);
  }
  
  detection_store_set_pending(manager->store, detection);
  detection_manager_increment_stat(manager, "queued");
}

guint detection_manager_process_pending(DetectionManager *manager)
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
  GList *ready_keys = NULL;
  
  g_hash_table_iter_init(&iter, manager->store->pending);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    Detection *detection = (Detection *)value;
    if (detection_manager_should_send(manager, detection, current_time)) {
      DetectionKey *dk = g_malloc(sizeof(DetectionKey));
      dk->source_id = detection->source_id;
      dk->object_id = detection->object_id;
      ready_keys = g_list_prepend(ready_keys, dk);
    }
  }
  
  pthread_mutex_unlock(&manager->store->pending_lock);
  
  for (GList *l = ready_keys; l != NULL; l = l->next) {
    DetectionKey *dk = (DetectionKey *)l->data;
    Detection *detection = detection_store_remove_pending(manager->store, dk->source_id, dk->object_id);
    
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
    g_free(dk);
  }
  
  g_list_free(ready_keys);
  return sent_count;
}

void detection_manager_print_stats(DetectionManager *manager)
{
  pthread_mutex_lock(&manager->stats.lock);
  g_print("INFO - Detection stats: queued=%u, sent=%u, skipped=%u, failed=%u, resent=%u, cleaned_sent=%u, cleaned_pending=%u\n",
          manager->stats.queued, manager->stats.sent, manager->stats.skipped,
          manager->stats.failed, manager->stats.resent,
          manager->stats.cleaned_sent, manager->stats.cleaned_pending);
  pthread_mutex_unlock(&manager->stats.lock);
}

gboolean detection_manager_init_kafka(DetectionManager *manager)
{
#ifdef KAFKA_ENABLED_BUILD
  if (!manager->broker || !manager->topic) {
    GST_WARNING("WARNING - Kafka broker or topic not configured\n");
    return FALSE;
  }
  
  manager->kafka_producer = kafka_producer_create(manager->broker);
  if (manager->kafka_producer) {
    manager->kafka_topic = kafka_topic_create(manager->kafka_producer, manager->topic);
    if (!manager->kafka_topic) {
      GST_WARNING("WARNING - Failed to create Kafka topic\n");
      kafka_producer_destroy(manager->kafka_producer, NULL);
      manager->kafka_producer = NULL;
      return FALSE;
    }
  } else {
    GST_WARNING("WARNING - Failed to create Kafka producer\n");
    return FALSE;
  }
  
  return TRUE;
#else
  GST_WARNING("WARNING - Kafka support not compiled in\n");
  return FALSE;
#endif
}

void detection_manager_cleanup_kafka(DetectionManager *manager)
{
#ifdef KAFKA_ENABLED_BUILD
  kafka_producer_destroy(manager->kafka_producer, manager->kafka_topic);
  manager->kafka_producer = NULL;
  manager->kafka_topic = NULL;
#endif
}

gboolean detection_manager_is_enabled(DetectionManager *manager)
{
  return manager ? manager->enabled : FALSE;
}

gboolean detection_manager_send_event(DetectionManager *manager,
                                      const gchar *event_type,
                                      const gchar *json_data)
{
  if (!manager || !event_type || !json_data)
    return FALSE;

  if (!manager->enabled) {
    GST_DEBUG("[send-event] manager disabled, skipping event_type=%s", event_type);
    return FALSE;
  }

#ifdef KAFKA_ENABLED_BUILD
  KafkaEvent event = {
    .event_type = event_type,
    .json_data  = json_data,
  };

  gboolean ok = kafka_send_event(manager, &event);
  detection_manager_increment_stat(manager, ok ? "sent" : "failed");
  return ok;
#else
  g_print("INFO - [send-event] Kafka not compiled in, dropping event_type=%s\n", event_type);
  return FALSE;
#endif
}