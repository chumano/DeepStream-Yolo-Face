#ifndef __UTILS_H__
#define __UTILS_H__


#include <glib.h>
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


#ifdef __cplusplus
}
#endif

#endif // __UTILS_H__