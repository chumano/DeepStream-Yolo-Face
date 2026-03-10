#ifndef __PIPELINE_MONITOR_H__
#define __PIPELINE_MONITOR_H__

#include <glib.h>
#include <gst/gst.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Constants
// =============================================================================

#define PIPELINE_MONITOR_MAX_SOURCES 32
#define PIPELINE_MONITOR_MAX_ELEMENTS 16
#define PIPELINE_MONITOR_LATENCY_HISTORY_SIZE 100

// =============================================================================
// Per-source frame statistics
// =============================================================================

typedef struct {
  /* Decoded frames emitted by the source (raw tee appsink, before streammux) */
  gint64 frames_from_source;
  /* Frames where PTS jumped by more than the expected interval (source-level drops) */
  gint64 frames_src_pts_gap;
  /* Frames arriving from this source (counted at appsink) */
  gint64 frames_received;
  /* Frames that had inference done */
  gint64 frames_inferred;
  /* Frames that contained at least one detected object */
  gint64 frames_with_detections;
  /* Number of objects detected across all frames */
  gint64 objects_detected;
  /* Number of "good quality" faces */
  gint64 good_faces;
  /* Number of "bad quality" faces */
  gint64 bad_faces;
  /* Sum of quality scores (for average computation) */
  gdouble quality_score_sum;
} PipelineSourceStats;

// =============================================================================
// Element queue metrics
// =============================================================================

typedef struct {
  const gchar *name;           /* Human-readable identifier */
  GstElement  *element;        /* Weak ref to the GstElement */
  /* Snapshot values (updated on each report interval) */
  guint        current_level_buffers;
  guint        max_size_buffers;
  guint64      current_level_bytes;
  guint64      max_size_bytes;
  guint        current_level_time_ms;
  /* Cumulative drops detected via overrun signal */
  gint64       buffers_dropped;
  /* Cumulative buffers that entered the queue (sink pad probe) */
  gint64       buffers_arrived;
  /* Cumulative buffers that left the queue (src pad probe) */
  gint64       buffers_passed;
} PipelineQueueMetrics;

// =============================================================================
// End-to-end latency tracking
// =============================================================================

typedef struct {
  gdouble samples[PIPELINE_MONITOR_LATENCY_HISTORY_SIZE];
  guint   head;           /* next write position (ring buffer) */
  guint   count;          /* number of valid samples */
  gdouble sum;            /* running sum of all samples (not ring-limited) */
  guint64 total_count;
  gdouble min_ms;
  gdouble max_ms;
} PipelineLatencyTracker;

// =============================================================================
// Metrics snapshot (safe to read from any thread after pipeline_monitor_snapshot)
// =============================================================================

typedef struct {
  GTimeVal           timestamp;
  guint              num_sources;
  PipelineSourceStats sources[PIPELINE_MONITOR_MAX_SOURCES];

  guint              num_queues;
  PipelineQueueMetrics queues[PIPELINE_MONITOR_MAX_ELEMENTS];

  gdouble            latency_min_ms;
  gdouble            latency_max_ms;
  gdouble            latency_avg_ms;   /* rolling window */
  gdouble            latency_avg_overall_ms;
  guint64            latency_sample_count;

  /* Whole-pipeline totals */
  gint64             total_frames_received;
  gint64             total_objects_detected;
  gint64             total_good_faces;
  gint64             total_bad_faces;
  gdouble            good_face_ratio;  /* good / (good + bad) */
  gdouble            avg_quality_score;

  /* Elapsed wall-clock seconds since monitor_start */
  gdouble            elapsed_sec;
} PipelineMonitorSnapshot;

// =============================================================================
// Opaque monitor handle
// =============================================================================

typedef struct _PipelineMonitor PipelineMonitor;

// =============================================================================
// API
// =============================================================================

/**
 * Create a new pipeline monitor.
 *
 * @param report_interval_sec  How often to print a metrics report (seconds).
 *                             Pass 0 to disable auto-reporting (call
 *                             pipeline_monitor_print_report() manually).
 * @param num_sources          Number of video sources in the pipeline.
 * @return New PipelineMonitor or NULL on failure.
 */
PipelineMonitor *pipeline_monitor_new(guint report_interval_sec,
                                      guint num_sources);

/**
 * Free the monitor and all resources.  Stops the periodic timer.
 */
void pipeline_monitor_free(PipelineMonitor *monitor);

/**
 * Register a GstQueue element to be polled for fill-level metrics.
 *
 * @param monitor  The monitor instance.
 * @param name     A short name used in reports (e.g. "queue_app").
 * @param element  The GstElement* for the queue.
 */
void pipeline_monitor_add_queue(PipelineMonitor *monitor,
                                const gchar     *name,
                                GstElement      *element);

/**
 * Record a decoded frame arriving directly from the source (raw tee appsink,
 * before nvstreammux).  Also detects PTS gaps that indicate upstream drops
 * (network packet loss, decoder drops, jitter-buffer discards).
 *
 * @param source_id      Source stream index.
 * @param pts_ns         GstClockTime PTS of the buffer (GST_CLOCK_TIME_NONE to skip gap check).
 * @param gap_threshold_ms  If the PTS jump exceeds this value (ms), count as a gap (0 → use 200 ms).
 */
void pipeline_monitor_record_source_frame(PipelineMonitor *monitor,
                                          guint            source_id,
                                          GstClockTime     pts_ns,
                                          gdouble          gap_threshold_ms);

/**
 * Record a processed frame for the given source.
 * Call once per frame from the appsink callback.
 *
 * @param infer_done TRUE if the frame had bInferDone set.
 * @param has_objects TRUE if the frame contained at least one detected object.
 */
void pipeline_monitor_record_frame(PipelineMonitor *monitor,
                                   guint            source_id,
                                   gboolean         infer_done,
                                   gboolean         has_objects);

/**
 * Record a single detected object / face.
 *
 * @param source_id    Source stream index.
 * @param is_good_face TRUE if the face passed quality checks.
 * @param quality_score Quality score in [0, 1].
 */
void pipeline_monitor_record_detection(PipelineMonitor *monitor,
                                       guint            source_id,
                                       gboolean         is_good_face,
                                       gdouble          quality_score);

/**
 * Record an end-to-end latency sample in milliseconds.
 * Called from the appsink callback using GstBuffer PTS vs. wall clock.
 */
void pipeline_monitor_record_latency(PipelineMonitor *monitor,
                                     gdouble          latency_ms);

/**
 * Fill a snapshot struct with the current (thread-safe) state.
 */
void pipeline_monitor_snapshot(PipelineMonitor         *monitor,
                                PipelineMonitorSnapshot *out);

/**
 * Print a formatted metrics report to GST_INFO / g_print.
 * Called automatically by the internal timer; also safe to call manually.
 */
void pipeline_monitor_print_report(PipelineMonitor *monitor);

#ifdef __cplusplus
}
#endif

#endif /* __PIPELINE_MONITOR_H__ */
