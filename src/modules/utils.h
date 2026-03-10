#ifndef __UTILS_H__
#define __UTILS_H__


#include <glib.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	guint content_w;
	guint content_h;
	guint pad_x;
	guint pad_y;
	gfloat scale;
} LetterboxGeometry;

LetterboxGeometry compute_letterbox_geometry(guint mux_w, guint mux_h,
     guint src_w, guint src_h);

/**
 * Format an NTP timestamp (nanoseconds) into a human-readable
 * string of the form "YYYY-MM-DD HH:MM:SS.mmm".
 *
 * @param ntp_ns     NTP timestamp in nanoseconds.
 * @param buf        Output buffer to write into.
 * @param buf_size   Size of @buf in bytes.
 */
void format_ntp_timestamp(guint64 ntp_ns, gchar *buf, gsize buf_size);


/**
 * Build a standardised JPEG filename for a frame saved after inference.
 *
 * Format: frame_src{source_id:02d}_num{frame_num:06d}_infer{0|1}_{YYYYmmdd_HHmmss_mmm}.jpg
 *
 * @param timestamp_sec  Frame timestamp in seconds (e.g. NTP ns / 1e9).
 *                       Pass 0 to fall back to the current wall-clock time.
 *
 * Zero-padded source_id and frame_num ensure lexicographic == temporal sort.
 * Milliseconds in the timestamp prevent collisions within the same second.
 * Returns a g_malloc()-allocated string; caller must g_free().
 */
gchar *frame_filename_new_infer(guint source_id, guint frame_num,
                                gboolean infer_done, gdouble timestamp_sec);

/**
 * Build a standardised JPEG filename for a pre-buffered frame.
 *
 * Format: frame_src{source_id:02d}_num{frame_num:06d}_{pts_us:012d}us.jpg
 *
 * The timestamp is encoded as integer microseconds to avoid decimal
 * separators in filenames.  Zero-padding keeps files in sort order.
 * Returns a g_malloc()-allocated string; caller must g_free().
 */
gchar *frame_filename_new_prebuf(guint source_id, guint frame_num,
                                 gdouble timestamp_sec);


#ifdef __cplusplus
}
#endif

#endif // __UTILS_H__