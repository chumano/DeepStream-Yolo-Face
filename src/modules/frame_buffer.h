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
 * @return Newly allocated FrameBuffer; free with frame_buffer_free().
 */
FrameBuffer *frame_buffer_new(guint num_sources,
                               gdouble pre_buffer_duration_sec,
                               const gchar *save_dir);

/**
 * Free a FrameBuffer and all buffered frame data.
 */
void frame_buffer_free(FrameBuffer *fb);

/**
 * Push an encoded JPEG frame into the ring buffer for @source_id.
 * The FrameBuffer takes ownership of @jpeg_data (allocated with g_malloc).
 *
 * @param fb         FrameBuffer instance
 * @param source_id  Source index
 * @param frame_num  Frame counter (used for file naming)
 * @param timestamp  Frame timestamp in seconds (used for pruning and naming)
 * @param jpeg_data  g_malloc()-allocated JPEG bytes (ownership transferred)
 * @param jpeg_size  Size of @jpeg_data in bytes
 */
void frame_buffer_push(FrameBuffer *fb, guint source_id, guint frame_num,
                       gdouble timestamp, guchar *jpeg_data, gsize jpeg_size);

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
 * Write all currently buffered frames for @source_id to disk and clear the
 * buffer.  Files are written to:
 *   {save_dir}/source_{source_id}/frame_src{source_id}_num{frame_num}_prebuf_{ts}.jpg
 *
 * @param fb        FrameBuffer instance
 * @param source_id Source index
 */
void frame_buffer_flush_to_disk(FrameBuffer *fb, guint source_id);

#ifdef __cplusplus
}
#endif

#endif /* __FRAME_BUFFER_H__ */
