/**
 * pipeline_monitor.c
 * ------------------
 * GStreamer pipeline element monitoring and metrics collection.
 *
 * Thread safety:
 *   All public functions that mutate state use a single GMutex.
 *   The periodic report timer callback also holds the lock while reading,
 *   so it is safe to call record_* functions from any GStreamer thread.
 */

#include "pipeline_monitor.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include <glib.h>
#include <gst/gst.h>

GST_DEBUG_CATEGORY_STATIC(pipeline_monitor_debug);
#define GST_CAT_DEFAULT pipeline_monitor_debug

// =============================================================================
// Internal types
// =============================================================================

typedef struct {
  PipelineMonitor *monitor;
  guint            queue_idx;
} QueueDropCtx;

// =============================================================================
// Internal struct
// =============================================================================

struct _PipelineMonitor {
  GMutex   lock;

  guint    num_sources;
  guint    report_interval_sec;

  /* Per-source counters – updated with g_atomic_int_add / g_atomic_int_inc */
  gint64       frames_from_source [PIPELINE_MONITOR_MAX_SOURCES]; /* raw decoded frames (tee appsink) */
  gint64       frames_src_pts_gap [PIPELINE_MONITOR_MAX_SOURCES]; /* PTS jump events (atomic) */
  GstClockTime last_src_pts       [PIPELINE_MONITOR_MAX_SOURCES]; /* last observed PTS (lock) */
  gint64   frames_received   [PIPELINE_MONITOR_MAX_SOURCES];
  gint64   frames_inferred   [PIPELINE_MONITOR_MAX_SOURCES];
  gint64   frames_with_dets  [PIPELINE_MONITOR_MAX_SOURCES];
  gint64   objects_detected  [PIPELINE_MONITOR_MAX_SOURCES];
  gint64   good_faces        [PIPELINE_MONITOR_MAX_SOURCES];
  gint64   bad_faces         [PIPELINE_MONITOR_MAX_SOURCES];
  gdouble  quality_score_sum [PIPELINE_MONITOR_MAX_SOURCES]; /* needs lock */

  /* Queue elements to poll */
  guint              num_queues;
  gchar             *queue_names         [PIPELINE_MONITOR_MAX_ELEMENTS];
  GstElement        *queue_elems         [PIPELINE_MONITOR_MAX_ELEMENTS];
  gint64             queue_drops         [PIPELINE_MONITOR_MAX_ELEMENTS]; /* cumulative (lock) */
  gulong             queue_signal_ids    [PIPELINE_MONITOR_MAX_ELEMENTS]; /* overrun handler id */
  gint64             queue_arrived       [PIPELINE_MONITOR_MAX_ELEMENTS]; /* buffers in  (atomic) */
  gint64             queue_passed        [PIPELINE_MONITOR_MAX_ELEMENTS]; /* buffers out (atomic) */
  gulong             queue_sink_probe_id [PIPELINE_MONITOR_MAX_ELEMENTS]; /* sink pad probe */
  gulong             queue_src_probe_id  [PIPELINE_MONITOR_MAX_ELEMENTS]; /* src  pad probe */

  /* Latency ring buffer (lock) */
  PipelineLatencyTracker latency;

  /* Timer */
  guint    timer_id;

  /* Start time */
  GTimeVal start_time;
};

// =============================================================================
// Queue overrun signal callback
// =============================================================================

static void
queue_overrun_cb(GstElement *queue G_GNUC_UNUSED, gpointer user_data)
{
  QueueDropCtx *ctx = (QueueDropCtx *)user_data;
  g_mutex_lock(&ctx->monitor->lock);
  ctx->monitor->queue_drops[ctx->queue_idx]++;
  g_mutex_unlock(&ctx->monitor->lock);
  GST_DEBUG("Queue[%u] overrun – buffer dropped (total drops: %"
            G_GINT64_FORMAT ")",
            ctx->queue_idx,
            ctx->monitor->queue_drops[ctx->queue_idx]);
}

// =============================================================================
// Queue pad probe callbacks (arrived / passed)
// =============================================================================

static GstPadProbeReturn
queue_sink_probe_cb(GstPad *pad G_GNUC_UNUSED, GstPadProbeInfo *info,
                    gpointer user_data)
{
  if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
    return GST_PAD_PROBE_OK;
  QueueDropCtx *ctx = (QueueDropCtx *)user_data;
  g_atomic_int_add((volatile gint *)&ctx->monitor->queue_arrived[ctx->queue_idx], 1);
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
queue_src_probe_cb(GstPad *pad G_GNUC_UNUSED, GstPadProbeInfo *info,
                   gpointer user_data)
{
  if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
    return GST_PAD_PROBE_OK;
  QueueDropCtx *ctx = (QueueDropCtx *)user_data;
  g_atomic_int_add((volatile gint *)&ctx->monitor->queue_passed[ctx->queue_idx], 1);
  return GST_PAD_PROBE_OK;
}

// =============================================================================
// Helpers
// =============================================================================

static gdouble
wall_time_sec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (gdouble)ts.tv_sec + (gdouble)ts.tv_nsec / 1e9;
}

static gdouble
timeval_diff_sec(const GTimeVal *start, const GTimeVal *end)
{
  return (gdouble)(end->tv_sec  - start->tv_sec)
       + (gdouble)(end->tv_usec - start->tv_usec) / 1e6;
}

// =============================================================================
// Latency tracker helpers (caller must hold lock)
// =============================================================================

static void
latency_record(PipelineLatencyTracker *t, gdouble ms)
{
  if (t->count == 0) {
    t->min_ms = ms;
    t->max_ms = ms;
  } else {
    if (ms < t->min_ms) t->min_ms = ms;
    if (ms > t->max_ms) t->max_ms = ms;
  }

  t->samples[t->head] = ms;
  t->head = (t->head + 1) % PIPELINE_MONITOR_LATENCY_HISTORY_SIZE;
  if (t->count < PIPELINE_MONITOR_LATENCY_HISTORY_SIZE) t->count++;

  t->sum += ms;
  t->total_count++;
}

static gdouble
latency_avg_window(const PipelineLatencyTracker *t)
{
  if (t->count == 0) return 0.0;
  gdouble sum = 0.0;
  for (guint i = 0; i < t->count; i++) {
    sum += t->samples[i];
  }
  return sum / (gdouble)t->count;
}

static gdouble
latency_avg_overall(const PipelineLatencyTracker *t)
{
  if (t->total_count == 0) return 0.0;
  return t->sum / (gdouble)t->total_count;
}

// =============================================================================
// Periodic report timer
// =============================================================================

static gboolean
monitor_report_callback(gpointer user_data)
{
  PipelineMonitor *m = (PipelineMonitor *)user_data;
  if (!m) return FALSE;
  pipeline_monitor_print_report(m);
  return TRUE; /* keep firing */
}

// =============================================================================
// Public API
// =============================================================================

PipelineMonitor *
pipeline_monitor_new(guint report_interval_sec, guint num_sources)
{
  GST_DEBUG_CATEGORY_INIT(pipeline_monitor_debug, "pipeline_monitor", 0,
                           "DeepStream Pipeline Monitor");

  g_return_val_if_fail(num_sources <= PIPELINE_MONITOR_MAX_SOURCES, NULL);

  PipelineMonitor *m = g_new0(PipelineMonitor, 1);
  g_mutex_init(&m->lock);

  m->num_sources          = num_sources;
  m->report_interval_sec  = report_interval_sec;

  memset(m->frames_from_source, 0, sizeof(m->frames_from_source));
  memset(m->frames_src_pts_gap, 0, sizeof(m->frames_src_pts_gap));
  for (guint i = 0; i < PIPELINE_MONITOR_MAX_SOURCES; i++)
    m->last_src_pts[i] = GST_CLOCK_TIME_NONE;
  memset(m->frames_received,    0, sizeof(m->frames_received));
  memset(m->frames_inferred,    0, sizeof(m->frames_inferred));
  memset(m->frames_with_dets,   0, sizeof(m->frames_with_dets));
  memset(m->objects_detected,   0, sizeof(m->objects_detected));
  memset(m->good_faces,         0, sizeof(m->good_faces));
  memset(m->bad_faces,          0, sizeof(m->bad_faces));
  memset(m->quality_score_sum,  0, sizeof(m->quality_score_sum));
  memset(&m->latency,           0, sizeof(PipelineLatencyTracker));

  g_get_current_time(&m->start_time);

  if (report_interval_sec > 0) {
    m->timer_id = g_timeout_add_seconds(report_interval_sec,
                                        monitor_report_callback, m);
  }

  GST_INFO("PipelineMonitor created (sources=%u, report_interval=%us)",
           num_sources, report_interval_sec);

  return m;
}

void
pipeline_monitor_free(PipelineMonitor *monitor)
{
  if (!monitor) return;

  if (monitor->timer_id) {
    g_source_remove(monitor->timer_id);
    monitor->timer_id = 0;
  }

  for (guint i = 0; i < monitor->num_queues; i++) {
    g_free(monitor->queue_names[i]);
    if (monitor->queue_elems[i]) {
      if (monitor->queue_signal_ids[i]) {
        g_signal_handler_disconnect(monitor->queue_elems[i],
                                    monitor->queue_signal_ids[i]);
        monitor->queue_signal_ids[i] = 0;
      }
      if (monitor->queue_sink_probe_id[i]) {
        GstPad *pad = gst_element_get_static_pad(monitor->queue_elems[i], "sink");
        if (pad) {
          gst_pad_remove_probe(pad, monitor->queue_sink_probe_id[i]);
          gst_object_unref(pad);
        }
        monitor->queue_sink_probe_id[i] = 0;
      }
      if (monitor->queue_src_probe_id[i]) {
        GstPad *pad = gst_element_get_static_pad(monitor->queue_elems[i], "src");
        if (pad) {
          gst_pad_remove_probe(pad, monitor->queue_src_probe_id[i]);
          gst_object_unref(pad);
        }
        monitor->queue_src_probe_id[i] = 0;
      }
      gst_object_unref(monitor->queue_elems[i]);
    }
  }

  g_mutex_clear(&monitor->lock);
  g_free(monitor);
}

void
pipeline_monitor_add_queue(PipelineMonitor *monitor,
                           const gchar     *name,
                           GstElement      *element)
{
  g_return_if_fail(monitor != NULL);
  g_return_if_fail(name    != NULL);
  g_return_if_fail(element != NULL);

  g_mutex_lock(&monitor->lock);

  if (monitor->num_queues >= PIPELINE_MONITOR_MAX_ELEMENTS) {
    GST_WARNING("PipelineMonitor: max queue slots reached, ignoring '%s'", name);
    g_mutex_unlock(&monitor->lock);
    return;
  }

  guint idx = monitor->num_queues++;
  monitor->queue_names[idx]         = g_strdup(name);
  monitor->queue_elems[idx]         = gst_object_ref(element);
  monitor->queue_drops[idx]         = 0;
  monitor->queue_signal_ids[idx]    = 0;
  monitor->queue_arrived[idx]       = 0;
  monitor->queue_passed[idx]        = 0;
  monitor->queue_sink_probe_id[idx] = 0;
  monitor->queue_src_probe_id[idx]  = 0;

  g_mutex_unlock(&monitor->lock);

  /* Connect overrun signal – fired each time the leaky queue drops a buffer */
  QueueDropCtx *ctx = g_new(QueueDropCtx, 1);
  ctx->monitor   = monitor;
  ctx->queue_idx = idx;
  monitor->queue_signal_ids[idx] = g_signal_connect_data(
      element, "overrun",
      G_CALLBACK(queue_overrun_cb),
      ctx, (GClosureNotify)g_free, 0);

  /* Sink pad probe – counts every buffer that arrives at the queue */
  GstPad *sink_pad = gst_element_get_static_pad(element, "sink");
  if (sink_pad) {
    QueueDropCtx *sctx = g_new(QueueDropCtx, 1);
    sctx->monitor   = monitor;
    sctx->queue_idx = idx;
    monitor->queue_sink_probe_id[idx] = gst_pad_add_probe(
        sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
        queue_sink_probe_cb, sctx, (GDestroyNotify)g_free);
    gst_object_unref(sink_pad);
  }

  /* Src pad probe – counts every buffer that leaves the queue */
  GstPad *src_pad = gst_element_get_static_pad(element, "src");
  if (src_pad) {
    QueueDropCtx *dctx = g_new(QueueDropCtx, 1);
    dctx->monitor   = monitor;
    dctx->queue_idx = idx;
    monitor->queue_src_probe_id[idx] = gst_pad_add_probe(
        src_pad, GST_PAD_PROBE_TYPE_BUFFER,
        queue_src_probe_cb, dctx, (GDestroyNotify)g_free);
    gst_object_unref(src_pad);
  }

  GST_DEBUG("PipelineMonitor: registered queue '%s' (overrun id=%lu, sink probe=%lu, src probe=%lu)",
            name, monitor->queue_signal_ids[idx],
            monitor->queue_sink_probe_id[idx],
            monitor->queue_src_probe_id[idx]);
}

void
pipeline_monitor_record_source_frame(PipelineMonitor *monitor,
                                     guint            source_id,
                                     GstClockTime     pts_ns,
                                     gdouble          gap_threshold_ms)
{
  if (!monitor) return;
  if (source_id >= PIPELINE_MONITOR_MAX_SOURCES) return;

  g_atomic_int_add((volatile gint *)&monitor->frames_from_source[source_id], 1);

  if (pts_ns == GST_CLOCK_TIME_NONE)
    return;

  gdouble threshold_ns = (gap_threshold_ms > 0.0 ? gap_threshold_ms : 200.0) * 1e6;

  g_mutex_lock(&monitor->lock);
  GstClockTime last = monitor->last_src_pts[source_id];
  monitor->last_src_pts[source_id] = pts_ns;
  g_mutex_unlock(&monitor->lock);

  if (last != GST_CLOCK_TIME_NONE && pts_ns > last) {
    gdouble gap_ns = (gdouble)(pts_ns - last);
    if (gap_ns > threshold_ns) {
      g_atomic_int_add((volatile gint *)&monitor->frames_src_pts_gap[source_id], 1);
      GST_WARNING("[src-drop] src=%u PTS gap %.1f ms (threshold %.0f ms) — likely dropped frame(s)",
                  source_id, gap_ns / 1e6, gap_threshold_ms > 0.0 ? gap_threshold_ms : 200.0);
    }
  }
}

void
pipeline_monitor_record_frame(PipelineMonitor *monitor,
                              guint            source_id,
                              gboolean         infer_done,
                              gboolean         has_objects)
{
  if (!monitor) return;
  if (source_id >= PIPELINE_MONITOR_MAX_SOURCES) return;

  g_atomic_int_add((volatile gint *)&monitor->frames_received[source_id], 1);
  if (infer_done) {
    g_atomic_int_add((volatile gint *)&monitor->frames_inferred[source_id], 1);
  }
  if (has_objects) {
    g_atomic_int_add((volatile gint *)&monitor->frames_with_dets[source_id], 1);
  }
}

void
pipeline_monitor_record_detection(PipelineMonitor *monitor,
                                  guint            source_id,
                                  gboolean         is_good_face,
                                  gdouble          quality_score)
{
  if (!monitor) return;
  if (source_id >= PIPELINE_MONITOR_MAX_SOURCES) return;

  g_atomic_int_add((volatile gint *)&monitor->objects_detected[source_id], 1);

  if (is_good_face) {
    g_atomic_int_add((volatile gint *)&monitor->good_faces[source_id], 1);
  } else {
    g_atomic_int_add((volatile gint *)&monitor->bad_faces[source_id], 1);
  }

  /* quality_score_sum is a double – needs explicit lock */
  g_mutex_lock(&monitor->lock);
  monitor->quality_score_sum[source_id] += quality_score;
  g_mutex_unlock(&monitor->lock);
}

void
pipeline_monitor_record_latency(PipelineMonitor *monitor, gdouble latency_ms)
{
  if (!monitor || isnan(latency_ms) || latency_ms < 0.0) return;

  g_mutex_lock(&monitor->lock);
  latency_record(&monitor->latency, latency_ms);
  g_mutex_unlock(&monitor->lock);
}

// =============================================================================
// Snapshot
// =============================================================================

void
pipeline_monitor_snapshot(PipelineMonitor         *monitor,
                           PipelineMonitorSnapshot *out)
{
  g_return_if_fail(monitor != NULL);
  g_return_if_fail(out     != NULL);

  memset(out, 0, sizeof(*out));

  GTimeVal now;
  g_get_current_time(&now);
  out->timestamp  = now;
  out->elapsed_sec = timeval_diff_sec(&monitor->start_time, &now);

  g_mutex_lock(&monitor->lock);

  out->num_sources = monitor->num_sources;

  for (guint i = 0; i < monitor->num_sources; i++) {
    out->sources[i].frames_from_source = monitor->frames_from_source[i];
    out->sources[i].frames_src_pts_gap = monitor->frames_src_pts_gap[i];
    out->sources[i].frames_received    = monitor->frames_received[i];
    out->sources[i].frames_inferred    = monitor->frames_inferred[i];
    out->sources[i].frames_with_detections = monitor->frames_with_dets[i];
    out->sources[i].objects_detected   = monitor->objects_detected[i];
    out->sources[i].good_faces         = monitor->good_faces[i];
    out->sources[i].bad_faces          = monitor->bad_faces[i];
    out->sources[i].quality_score_sum  = monitor->quality_score_sum[i];

    out->total_frames_received   += out->sources[i].frames_received;
    out->total_objects_detected  += out->sources[i].objects_detected;
    out->total_good_faces        += out->sources[i].good_faces;
    out->total_bad_faces         += out->sources[i].bad_faces;
  }

  /* Good-face ratio */
  gint64 total_faces = out->total_good_faces + out->total_bad_faces;
  out->good_face_ratio = (total_faces > 0)
      ? (gdouble)out->total_good_faces / (gdouble)total_faces
      : 0.0;

  /* Average quality score */
  gdouble total_score_sum = 0.0;
  for (guint i = 0; i < monitor->num_sources; i++) {
    total_score_sum += monitor->quality_score_sum[i];
  }
  out->avg_quality_score = (out->total_objects_detected > 0)
      ? total_score_sum / (gdouble)out->total_objects_detected
      : 0.0;

  /* Latency */
  out->latency_min_ms         = monitor->latency.min_ms;
  out->latency_max_ms         = monitor->latency.max_ms;
  out->latency_avg_ms         = latency_avg_window(&monitor->latency);
  out->latency_avg_overall_ms = latency_avg_overall(&monitor->latency);
  out->latency_sample_count   = monitor->latency.total_count;

  /* Queue levels – poll live via GObject properties */
  out->num_queues = monitor->num_queues;
  for (guint i = 0; i < monitor->num_queues; i++) {
    PipelineQueueMetrics *qm = &out->queues[i];
    qm->name            = monitor->queue_names[i]; /* static lifetime */
    qm->element         = monitor->queue_elems[i];
    qm->buffers_dropped = monitor->queue_drops[i];
    qm->buffers_arrived = monitor->queue_arrived[i];
    qm->buffers_passed  = monitor->queue_passed[i];

    GstElement *q = monitor->queue_elems[i];
    if (q) {
      guint  cur_bufs   = 0;
      guint  max_bufs   = 0;
      guint64 cur_bytes = 0;
      guint64 max_bytes = 0;
      guint64 cur_time  = 0;

      g_object_get(q,
          "current-level-buffers", &cur_bufs, //  Current number of buffers in the queue
          "max-size-buffers",      &max_bufs,
          "current-level-bytes",   &cur_bytes, //  Current amount of data in the queue (in bytes)
          "max-size-bytes",        &max_bytes, 
          "current-level-time",    &cur_time, //  Current amount of data in the queue (in ns)
          NULL);

      qm->current_level_buffers  = cur_bufs;
      qm->max_size_buffers       = max_bufs;
      qm->current_level_bytes    = cur_bytes;
      qm->max_size_bytes         = max_bytes;
      qm->current_level_time_ms  = (guint)(cur_time / 1000000ULL);
    }
  }

  g_mutex_unlock(&monitor->lock);
}

// =============================================================================
// Report printing
// =============================================================================

void
pipeline_monitor_print_report(PipelineMonitor *monitor)
{
  if (!monitor) return;

  PipelineMonitorSnapshot snap;
  pipeline_monitor_snapshot(monitor, &snap);

  /* Wall-clock string */
  time_t now_t = (time_t)snap.timestamp.tv_sec;
  struct tm *tm_info = localtime(&now_t);
  char time_str[32];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
  gint msec = (gint)(snap.timestamp.tv_usec / 1000);

  g_print("\n");
  g_print("╔══════════════════════════════════════════════════════════════╗\n");
  g_print("║          PIPELINE METRICS  [%s.%03d]           ║\n", time_str, msec);
  g_print("╠══════════════════════════════════════════════════════════════╣\n");
  g_print("║  Elapsed:  %.1f s                                             \n", snap.elapsed_sec);
  g_print("╠══════════════════════════════════════════════════════════════╣\n");

  /* Per-source stats */
  g_print("║  SOURCE STATISTICS                                           \n");
  g_print("║  %-8s  %10s  %8s  %12s  %12s  %12s  %8s\n",
          "Source", "SrcFrames", "PtsGaps", "->Mux(drop%)", "Inf(%)", "Det(%)", "Objects");
  for (guint i = 0; i < snap.num_sources; i++) {
    const PipelineSourceStats *s = &snap.sources[i];
    gdouble infer_ratio = (s->frames_received > 0)
        ? 100.0 * (gdouble)s->frames_inferred / (gdouble)s->frames_received : 0.0;
    gdouble det_ratio   = (s->frames_inferred > 0)
        ? 100.0 * (gdouble)s->frames_with_detections / (gdouble)s->frames_inferred : 0.0;
    /* frames dropped between decoded output and streammux appsink */
    gint64  pre_mux_drop = (s->frames_from_source > s->frames_received)
        ? s->frames_from_source - s->frames_received : 0;
    gdouble drop_pct = (s->frames_from_source > 0)
        ? 100.0 * (gdouble)pre_mux_drop / (gdouble)s->frames_from_source : 0.0;

    g_print("║  src[%2u]  %10"G_GINT64_FORMAT"  %8"G_GINT64_FORMAT
            "  %8"G_GINT64_FORMAT"(-%4.1f%%)  %8"G_GINT64_FORMAT"(%5.1f%%)"
            "  %8"G_GINT64_FORMAT"(%5.1f%%)  %8"G_GINT64_FORMAT"\n",
            i,
            s->frames_from_source,
            s->frames_src_pts_gap,
            s->frames_received, drop_pct,
            s->frames_inferred, infer_ratio,
            s->frames_with_detections, det_ratio,
            s->objects_detected);
  }

  g_print("╠══════════════════════════════════════════════════════════════╣\n");

  /* Face quality */
  g_print("║  FACE QUALITY                                                \n");
  g_print("║  Total det: %"G_GINT64_FORMAT"  Good: %"G_GINT64_FORMAT
          "  Bad: %"G_GINT64_FORMAT"  Ratio: %.1f%%  Avg score: %.4f\n",
          snap.total_objects_detected,
          snap.total_good_faces,
          snap.total_bad_faces,
          snap.good_face_ratio * 100.0,
          snap.avg_quality_score);

  /* Per-source face details */
  for (guint i = 0; i < snap.num_sources; i++) {
    const PipelineSourceStats *s = &snap.sources[i];
    gint64 faces = s->good_faces + s->bad_faces;
    if (faces == 0) continue;
    gdouble avg_q = (s->objects_detected > 0)
        ? s->quality_score_sum / (gdouble)s->objects_detected : 0.0;
    gdouble g_ratio = (gdouble)s->good_faces / (gdouble)faces * 100.0;
    g_print("║    src[%2u]  good=%"G_GINT64_FORMAT" (%.1f%%)  bad=%"G_GINT64_FORMAT
            "  avg_score=%.4f\n",
            i, s->good_faces, g_ratio, s->bad_faces, avg_q);
  }

  g_print("╠══════════════════════════════════════════════════════════════╣\n");

  /* Latency */
  if (snap.latency_sample_count > 0) {
    g_print("║  END-TO-END LATENCY (buffer PTS -> appsink processing)       \n");
    g_print("║  Samples: %"G_GUINT64_FORMAT"  Min: %.1fms  Max: %.1fms"
            "  Avg(win): %.1fms  Avg(all): %.1fms\n",
            snap.latency_sample_count,
            snap.latency_min_ms,
            snap.latency_max_ms,
            snap.latency_avg_ms,
            snap.latency_avg_overall_ms);
    g_print("╠══════════════════════════════════════════════════════════════╣\n");
  }

  /* Queue levels */
  if (snap.num_queues > 0) {
    g_print("║  QUEUE LEVELS                                                \n");
    g_print("║  %-16s  %8s/%8s  %20s  %12s  %8s\n",
            "Queue", "Bufs", "MaxBufs", "Arrived", "Passed", "Drops");
    for (guint i = 0; i < snap.num_queues; i++) {
      const PipelineQueueMetrics *qm = &snap.queues[i];
      gdouble fill_pct = (qm->max_size_buffers > 0)
          ? 100.0 * (gdouble)qm->current_level_buffers / (gdouble)qm->max_size_buffers
          : 0.0;
      /* drop rate as % of arrived buffers */
      gdouble drop_pct = (qm->buffers_arrived > 0)
          ? 100.0 * (gdouble)qm->buffers_dropped / (gdouble)qm->buffers_arrived
          : 0.0;

      g_print("║  %-16s  %8u/%8u (%5.1f%%)  %12"
              G_GINT64_FORMAT"  %12"G_GINT64_FORMAT"  %8"G_GINT64_FORMAT" (%4.1f%%)"
              "\n",
              qm->name,
              qm->current_level_buffers, qm->max_size_buffers, fill_pct,
              qm->buffers_arrived,
              qm->buffers_passed,
              qm->buffers_dropped, drop_pct);
    }
    g_print("╠══════════════════════════════════════════════════════════════╣\n");
  }

  g_print("╚══════════════════════════════════════════════════════════════╝\n");
  g_print("\n");
}
