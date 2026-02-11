#ifndef __JSON_BUILDER_H__
#define __JSON_BUILDER_H__

#include <glib.h>
#include "face_analysis.h"
#include "image_processing.h"
#include "face.h"


/**
 * Build JSON string from face detection context
 */
gchar *build_detection_json(FaceContext *ctx);

#endif
