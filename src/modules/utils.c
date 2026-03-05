#include "utils.h"

#define MIN(a,b) ((a)<(b)?(a):(b))

// Compute letterbox geometry
LetterboxGeometry compute_letterbox_geometry(guint mux_w, guint mux_h,
     guint src_w, guint src_h) {
    // guint mux_w  = surface->surfaceList[batch_id].width;
    // guint mux_h  = surface->surfaceList[batch_id].height;
    // guint src_w  = frame_meta->source_frame_width;
    // guint src_h  = frame_meta->source_frame_height;

	LetterboxGeometry geom;
	geom.scale = MIN((gfloat)mux_w / src_w, (gfloat)mux_h / src_h);
	geom.content_w = (guint)(src_w * geom.scale);
	geom.content_h = (guint)(src_h * geom.scale);
	geom.pad_x = (mux_w - geom.content_w) / 2;
	geom.pad_y = (mux_h - geom.content_h) / 2;
	return geom;
}
