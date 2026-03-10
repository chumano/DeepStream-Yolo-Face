#include "frame_buffer.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <glib/gstdio.h>
#include "gstnvdsmeta.h"

// Reuse the debug category defined in deepstream.cpp
GST_DEBUG_CATEGORY_EXTERN(deepstream_debug_category);
#define GST_CAT_DEFAULT deepstream_debug_category

// =============================================================================
// Internal types
// =============================================================================

typedef struct {
  guint    frame_num;
  gdouble  timestamp;   /**< seconds */
  guchar  *jpeg_data;   /**< owned, g_malloc()-allocated */
  gsize    jpeg_size;
} BufferedFrame;

typedef struct {
  GQueue  *frames;     /**< queue<BufferedFrame *>, head = newest */
  GMutex   mutex;
  guint    source_id;
} SourceBuffer;

/** Task pushed onto the async save queue by frame_buffer_save_frame(). */
typedef struct {
  guint   source_id;
  gdouble pts_sec;
  guint   retry_count;  /**< number of times this task has been retried */
} SaveTask;

/** Max number of retries when the target frame hasn't arrived yet. */
#define MAX_SAVE_RETRIES 10
/** Sleep between retries (microseconds). */
#define RETRY_SLEEP_US   100000   /* 100 ms */

/** Sentinel value placed on the queue to stop the worker thread cleanly. */
static SaveTask _save_task_sentinel;
#define SAVE_TASK_SENTINEL (&_save_task_sentinel)

struct _FrameBuffer {
  SourceBuffer  *sources;              /**< array [0..num_sources-1] */
  guint          num_sources;
  gdouble        pre_buffer_duration_sec;
  gchar         *save_dir;             /**< base output directory, owned */

  /* Async save worker */
  GAsyncQueue   *save_queue;           /**< thread-safe task queue */
  GThread       *worker_thread;        /**< background save thread */
};

// =============================================================================
// Helpers
// =============================================================================

static void
buffered_frame_free(gpointer data)
{
  if (!data)
    return;
  BufferedFrame *f = (BufferedFrame *)data;
  g_free(f->jpeg_data);
  g_free(f);
}

/**
 * Build the absolute file path for a pre-buffered frame.
 * Directory: {save_dir}/source_{source_id}/
 * Filename:  see frame_filename_new_prebuf() in utils.h
 *
 * Returns a g_malloc()-allocated string; caller must g_free().
 */
static gchar *
build_prebuf_path(const gchar *save_dir, guint source_id,
                  guint frame_num, gdouble timestamp)
{
  gchar *src_dir = g_strdup_printf("%s/source_%02u", save_dir, source_id);

  /* Ensure per-source directory exists */
  if (g_mkdir_with_parents(src_dir, 0755) != 0 &&
      !g_file_test(src_dir, G_FILE_TEST_IS_DIR)) {
    GST_ERROR("frame_buffer: failed to create directory %s", src_dir);
    g_free(src_dir);
    return NULL;
  }

  gchar *filename = frame_filename_new_prebuf(source_id, frame_num, timestamp);
  gchar *path     = g_strdup_printf("%s/%s", src_dir, filename);
  g_free(filename);
  g_free(src_dir);
  return path;
}

// =============================================================================
// Worker thread
// =============================================================================

/**
 * Execute one save task: find the frame(s) in the source ring buffer whose
 * PTS matches task->pts_sec (±1 ms) and write them to disk, then discard
 * all frames that are at or before that PTS.
 *
 * Returns TRUE if the task should be retried (target frame not yet arrived),
 * FALSE when the task is fully handled (saved or definitively not found).
 */
static gboolean
worker_execute_save(FrameBuffer *fb, const SaveTask *task)
{
  guint   source_id = task->source_id;
  gdouble pts_sec   = task->pts_sec;

  if (source_id >= fb->num_sources)
    return FALSE;

  const gdouble kTolerance = 0.001;

  SourceBuffer *sb = &fb->sources[source_id];

  g_mutex_lock(&sb->mutex);

  /*
   * Early-exit: check whether the newest buffered frame has already passed
   * the target PTS.  If the ring buffer is empty, or the newest (head) frame
   * is still older than pts_sec, the raw-tee thread hasn't pushed the target
   * frame yet — signal the caller to retry after a short sleep.
   */
  if (g_queue_is_empty(sb->frames)) {
    g_mutex_unlock(&sb->mutex);
    GST_WARNING("frame_buffer: [async] ring buffer empty for src=%u, will retry (attempt=%u)",
              source_id, task->retry_count + 1);
    return TRUE;
  }

  BufferedFrame *newest = (BufferedFrame *)g_queue_peek_head(sb->frames);
  if (newest->timestamp < pts_sec - kTolerance) {
    g_mutex_unlock(&sb->mutex);
    GST_INFO("frame_buffer: [async] newest frame pts=%.6f < target pts=%.6f, will retry src=%u (attempt=%u)",
              newest->timestamp, pts_sec, source_id, task->retry_count + 1);
    return TRUE;
  }

  guint flushed = 0;

  GList *link = sb->frames->tail;  /* start from oldest */
  while (link) {
    GList *current = link;
    link = link->prev;

    BufferedFrame *f = (BufferedFrame *)current->data;

    if (f->timestamp <= pts_sec + kTolerance) {
      if (f->timestamp >= pts_sec - kTolerance) {
        /* PTS match — save to disk */
        gchar *abs_path = build_prebuf_path(fb->save_dir, source_id,
                                            f->frame_num, f->timestamp);
        if (abs_path) {
          FILE *fp = fopen(abs_path, "wb");
          if (fp) {
            fwrite(f->jpeg_data, 1, f->jpeg_size, fp);
            fclose(fp);
            GST_INFO("frame_buffer: [async] saved pts-matched frame src=%u num=%u pts=%.6f -> %s",
                      source_id, f->frame_num, f->timestamp, abs_path);
            flushed++;
          } else {
            GST_WARNING("frame_buffer: [async] failed to open %s for writing", abs_path);
          }
          g_free(abs_path);
        }
      } else {
        GST_DEBUG("frame_buffer: [async] dropping old frame src=%u num=%u pts=%.6f (detection pts=%.6f)",
                  source_id, f->frame_num, f->timestamp, pts_sec);
      }
      g_queue_unlink(sb->frames, current);
      buffered_frame_free(f);
      g_list_free(current);
    }
  }

  g_mutex_unlock(&sb->mutex);

  if (flushed > 0)
    GST_DEBUG("frame_buffer: [async] saved %u frame(s) for source %u (pts=%.6f)",
              flushed, source_id, pts_sec);
  else
    GST_WARNING("frame_buffer: [async] no matching frame for source %u (pts=%.6f) — frame may have been pruned",
                source_id, pts_sec);

  return FALSE;
}

static gpointer
worker_thread_func(gpointer user_data)
{
  FrameBuffer *fb = (FrameBuffer *)user_data;

  GST_INFO("frame_buffer: async save worker started");

  while (TRUE) {
    /* Block indefinitely until a task is available */
    SaveTask *task = (SaveTask *)g_async_queue_pop(fb->save_queue);

    if (task == SAVE_TASK_SENTINEL) {
      /* Shutdown signal received */
      GST_INFO("frame_buffer: async save worker stopping");
      break;
    }

    gboolean needs_retry = worker_execute_save(fb, task);
    if (needs_retry && task->retry_count < MAX_SAVE_RETRIES) {
      task->retry_count++;
      g_usleep(RETRY_SLEEP_US);
      g_async_queue_push(fb->save_queue, task);  /* requeue — do NOT free */
    } else {
      if (needs_retry) {
        GST_WARNING("frame_buffer: [async] giving up on src=%u pts=%.6f after %u retries — frame never arrived",
                    task->source_id, task->pts_sec, task->retry_count);
      }
      g_free(task);
    }
  }

  return NULL;
}

// =============================================================================
// Public API
// =============================================================================

FrameBuffer *
frame_buffer_new(guint num_sources, gdouble pre_buffer_duration_sec,
                 const gchar *save_dir)
{
  if (num_sources == 0 || !save_dir) {
    GST_ERROR("frame_buffer_new: invalid arguments");
    return NULL;
  }

  FrameBuffer *fb = g_new0(FrameBuffer, 1);
  fb->num_sources            = num_sources;
  fb->pre_buffer_duration_sec = pre_buffer_duration_sec;
  fb->save_dir               = g_strdup(save_dir);
  fb->sources                = g_new0(SourceBuffer, num_sources);

  for (guint i = 0; i < num_sources; i++) {
    fb->sources[i].source_id = i;
    fb->sources[i].frames    = g_queue_new();
    g_mutex_init(&fb->sources[i].mutex);
  }

  /* Start async save worker */
  fb->save_queue     = g_async_queue_new();
  fb->worker_thread  = g_thread_new("fb-save-worker", worker_thread_func, fb);

  GST_INFO("frame_buffer: created (sources=%u, pre_buffer=%.2fs, dir=%s)",
           num_sources, pre_buffer_duration_sec, save_dir);
  return fb;
}

void
frame_buffer_free(FrameBuffer *fb)
{
  if (!fb)
    return;

  /* Signal the worker thread to stop and wait for it to finish */
  if (fb->worker_thread) {
    g_async_queue_push(fb->save_queue, SAVE_TASK_SENTINEL);
    g_thread_join(fb->worker_thread);
    fb->worker_thread = NULL;
  }

  if (fb->save_queue) {
    /* Drain any leftover tasks that were never consumed */
    SaveTask *leftover;
    while ((leftover = (SaveTask *)g_async_queue_try_pop(fb->save_queue)) != NULL)
      g_free(leftover);
    g_async_queue_unref(fb->save_queue);
    fb->save_queue = NULL;
  }

  for (guint i = 0; i < fb->num_sources; i++) {
    SourceBuffer *sb = &fb->sources[i];
    g_mutex_lock(&sb->mutex);
    g_queue_free_full(sb->frames, buffered_frame_free);
    sb->frames = NULL;
    g_mutex_unlock(&sb->mutex);
    g_mutex_clear(&sb->mutex);
  }

  g_free(fb->sources);
  g_free(fb->save_dir);
  g_free(fb);
}

void
frame_buffer_push(FrameBuffer *fb, guint source_id, guint frame_num,
                  gdouble timestamp, guchar *jpeg_data, gsize jpeg_size)
{
  if (!fb || !jpeg_data || jpeg_size == 0) {
    g_free(jpeg_data);
    return;
  }
  if (source_id >= fb->num_sources) {
    GST_WARNING("frame_buffer_push: source_id %u >= num_sources %u",
                source_id, fb->num_sources);
    g_free(jpeg_data);
    return;
  }

  BufferedFrame *f = g_new0(BufferedFrame, 1);
  f->frame_num  = frame_num;
  f->timestamp  = timestamp;
  f->jpeg_data  = jpeg_data;  /* take ownership */
  f->jpeg_size  = jpeg_size;

  SourceBuffer *sb = &fb->sources[source_id];
  g_mutex_lock(&sb->mutex);
  g_queue_push_head(sb->frames, f);  /* head = newest */
  g_mutex_unlock(&sb->mutex);
}

void
frame_buffer_prune(FrameBuffer *fb, guint source_id, gdouble current_time)
{
  if (!fb || source_id >= fb->num_sources)
    return;

  gdouble cutoff = current_time - fb->pre_buffer_duration_sec;
  SourceBuffer *sb = &fb->sources[source_id];

  g_mutex_lock(&sb->mutex);
  /* Tail of queue = oldest frames; remove while older than cutoff */
  while (!g_queue_is_empty(sb->frames)) {
    BufferedFrame *tail = (BufferedFrame *)g_queue_peek_tail(sb->frames);
    if (tail->timestamp < cutoff) {
      g_queue_pop_tail(sb->frames);
      buffered_frame_free(tail);
    } else {
      break;
    }
  }
  g_mutex_unlock(&sb->mutex);
}

void
frame_buffer_save_frame(FrameBuffer *fb, guint source_id, gdouble pts_sec)
{
  if (!fb || source_id >= fb->num_sources || !fb->save_queue)
    return;

  /* Non-blocking: enqueue a save task and return immediately.
   * The actual disk I/O is handled by the background worker thread. */
  SaveTask *task = g_new0(SaveTask, 1);
  task->source_id = source_id;
  task->pts_sec   = pts_sec;

  g_async_queue_push(fb->save_queue, task);

  GST_DEBUG("frame_buffer: queued save task src=%u pts=%.6f (queue_len=%d)",
            source_id, pts_sec, g_async_queue_length(fb->save_queue));
}
