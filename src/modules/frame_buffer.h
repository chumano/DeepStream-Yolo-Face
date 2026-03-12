#ifndef __FRAME_BUFFER_H__
#define __FRAME_BUFFER_H__

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * FrameBuffer — per-source ring buffer of recently captured JPEG frames.
 *
 * Frames are stored in memory (as encoded JPEG data) and pruned
 * automatically based on a configurable time window.  When a detection
 * event occurs the caller can flush all buffered frames to disk so that
 * the images captured *before* the detection are preserved.
 *
 * Thread-safety: each source's sub-buffer is protected by its own mutex;
 * it is safe to push/prune/flush from different threads as long as each
 * call targets a different source_id.
 */

typedef struct _FrameBuffer FrameBuffer;

/**
 * Create a new FrameBuffer.
 *
 * @param num_sources            Number of video sources (source_id range: 0..num_sources-1)
 * @param pre_buffer_duration_sec Seconds of frames to keep in memory per source
 * @param save_dir               Base output directory (e.g. /app/outputs/frames)
 * @param jpeg_quality           JPEG quality (1-100) used when encoding frames for disk
 * @return Newly allocated FrameBuffer; free with frame_buffer_free().
 */
FrameBuffer *frame_buffer_new(guint num_sources,
                               gdouble pre_buffer_duration_sec,
                               const gchar *save_dir,
                               gint jpeg_quality);

/**
 * Free a FrameBuffer and all buffered frame data.
 */
void frame_buffer_free(FrameBuffer *fb);

/**
 * Push a raw CPU RGBA frame into the ring buffer for @source_id.
 * The FrameBuffer takes ownership of @rgba_data (allocated with g_malloc).
 * JPEG encoding is deferred to the background worker thread and only
 * performed when the frame is actually selected for saving.
 *
 * @param fb         FrameBuffer instance
 * @param source_id  Source index
 * @param frame_num  Frame counter (used for file naming)
 * @param timestamp  Frame timestamp in seconds (used for pruning and naming)
 * @param rgba_data  g_malloc()-allocated CPU RGBA buffer (ownership transferred)
 * @param width      Frame width in pixels
 * @param height     Frame height in pixels
 * @param pitch      Row stride in bytes (may be > width*4)
 */
void frame_buffer_push(FrameBuffer *fb, guint source_id, guint frame_num,
                       gdouble timestamp, guchar *rgba_data,
                       guint width, guint height, guint pitch);

/**
 * Remove frames older than (current_time - pre_buffer_duration_sec) from
 * the ring buffer for @source_id.
 *
 * @param fb           FrameBuffer instance
 * @param source_id    Source index
 * @param current_time Wall-clock / stream time in seconds
 */
void frame_buffer_prune(FrameBuffer *fb, guint source_id, gdouble current_time);

/**
 * Enqueue an async save task for @source_id at @pts_sec.
 *
 * This function returns immediately after placing the request on an internal
 * work queue.  A background worker thread picks up the task and:
 *   - saves frame(s) whose timestamp matches @pts_sec (within 1 ms) to disk,
 *   - discards all buffered frames with timestamp <= @pts_sec.
 *
 * Files are written to:
 *   {save_dir}/source_{source_id}/frame_src{source_id}_num{frame_num}_prebuf_{ts}.jpg
 *
 * @param fb        FrameBuffer instance
 * @param source_id Source index
 * @param pts_sec   Detection frame PTS in seconds
 */
void frame_buffer_save_frame(FrameBuffer *fb, guint source_id, gdouble pts_sec);

/**
 * Log current frame-buffer metrics for all sources via GST_INFO.
 *
 * Prints per-source stats (frames in ring buffer, memory used in KB,
 * total pushed/saved/pruned) plus aggregate totals and the save-queue
 * length.  Safe to call from any thread.
 */
void frame_buffer_log_metrics(FrameBuffer *fb);

/**
 * Change the interval at which metrics are automatically logged.
 *
 * The background metrics thread wakes every @interval_sec seconds and
 * calls frame_buffer_log_metrics().  Pass 0 to disable periodic logging
 * (manual calls to frame_buffer_log_metrics() still work).
 *
 * Default: 30 seconds.
 *
 * @param fb           FrameBuffer instance
 * @param interval_sec Logging interval in seconds (0 = disable)
 */
void frame_buffer_set_metrics_interval(FrameBuffer *fb, guint interval_sec);

#ifdef __cplusplus
}
#endif

#endif /* __FRAME_BUFFER_H__ */
