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


#ifdef __cplusplus
}
#endif

#endif // __UTILS_H__