#include "frame_buffer.h"
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

struct _FrameBuffer {
  SourceBuffer  *sources;              /**< array [0..num_sources-1] */
  guint          num_sources;
  gdouble        pre_buffer_duration_sec;
  gchar         *save_dir;             /**< base output directory, owned */
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
 * Build absolute file path for a pre-buffered frame.
 * Pattern: {save_dir}/source_{source_id}/frame_src{source_id}_num{frame_num}_prebuf_{ts_us}.jpg
 * Returns g_malloc()-allocated string; caller must g_free().
 */
static gchar *
build_prebuf_path(const gchar *save_dir, guint source_id,
                  guint frame_num, gdouble timestamp)
{
  gchar *src_dir = g_strdup_printf("%s/source_%u", save_dir, source_id);

  /* Ensure per-source directory exists */
  if (g_mkdir_with_parents(src_dir, 0755) != 0 &&
      !g_file_test(src_dir, G_FILE_TEST_IS_DIR)) {
    GST_ERROR("frame_buffer: failed to create directory %s", src_dir);
    g_free(src_dir);
    return NULL;
  }

  /* Encode timestamp as integer microseconds to avoid dots in filename */
  guint64 ts_us = (guint64)(timestamp * 1e6);
  gchar *path = g_strdup_printf("%s/frame_src%u_num%u_prebuf_%" G_GUINT64_FORMAT ".jpg",
                                src_dir, source_id, frame_num, ts_us);
  g_free(src_dir);
  return path;
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

  GST_INFO("frame_buffer: created (sources=%u, pre_buffer=%.2fs, dir=%s)",
           num_sources, pre_buffer_duration_sec, save_dir);
  return fb;
}

void
frame_buffer_free(FrameBuffer *fb)
{
  if (!fb)
    return;

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
frame_buffer_flush_to_disk(FrameBuffer *fb, guint source_id)
{
  if (!fb || source_id >= fb->num_sources)
    return;

  SourceBuffer *sb = &fb->sources[source_id];

  g_mutex_lock(&sb->mutex);

  guint flushed = 0;
  GList *link = sb->frames->tail;  /* start from oldest */
  while (link) {
    BufferedFrame *f = (BufferedFrame *)link->data;
    gchar *abs_path = build_prebuf_path(fb->save_dir, source_id,
                                        f->frame_num, f->timestamp);
    if (abs_path) {
      FILE *fp = fopen(abs_path, "wb");
      if (fp) {
        fwrite(f->jpeg_data, 1, f->jpeg_size, fp);
        fclose(fp);
        GST_DEBUG("frame_buffer: flushed pre-buf frame src=%u num=%u -> %s",
                  source_id, f->frame_num, abs_path);
        flushed++;
      } else {
        GST_WARNING("frame_buffer: failed to open %s for writing", abs_path);
      }
      g_free(abs_path);
    }
    link = link->prev;  /* move towards newest */
  }

  /* Clear the buffer after flushing */
  g_queue_free_full(sb->frames, buffered_frame_free);
  sb->frames = g_queue_new();

  g_mutex_unlock(&sb->mutex);

  if (flushed > 0)
    GST_DEBUG("frame_buffer: flushed %u pre-detection frames for source %u",
             flushed, source_id);
}
