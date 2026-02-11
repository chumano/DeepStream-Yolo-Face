#ifndef __FACE_ANALYSIS_H__
#define __FACE_ANALYSIS_H__

#include <glib.h>
#include "gstnvdsmeta.h"
#include "face.h"



/**
 * Assess face quality based on landmarks
 */
gboolean assess_face_quality(Landmark *landmarks, guint num_landmarks, 
                             gboolean *is_good_face, gdouble *quality_score, 
                             FaceQualityMetrics *metrics);

/**
 * Extract landmarks from object metadata
 */
Landmark *extract_landmarks_from_object(NvDsObjectMeta *obj_meta, guint *num_landmarks_out);

#endif
