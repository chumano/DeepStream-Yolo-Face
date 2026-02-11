#ifndef __DEEPSTREAM_H__
#define __DEEPSTREAM_H__

#include <nvdsgstutils.h>
#include <cuda_runtime_api.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "gstnvdsmeta.h"
#include "nvbufsurface.h"

#include "modules/interrupt.h"
#include "modules/perf.h"
#include "detection_manager.h"

// =============================================================================
// Application Structures
// =============================================================================

typedef struct {
  guint left;
  guint top;
  guint width;
  guint height;
} CropBox;

typedef struct _FaceQualityMetrics {
  gboolean is_frontal;
  guint visible_landmarks;
  guint total_landmarks;
  gdouble avg_confidence;
  gdouble quality_score;
  gdouble frontal_score;
} FaceQualityMetrics;

typedef struct _Landmark {
  gdouble x;
  gdouble y;
  gdouble confidence;
} Landmark;

typedef struct _FaceContext {
  guint source_id;
  gdouble frame_timestamp;
  guint frame_num;
  guint64 object_id;
  gint class_id;
  gdouble confidence;

  const Landmark *landmarks;
  guint num_landmarks;
  const gchar* frame_image_path;

  const CropBox* bbox;
  const CropBox* crop_box;
  gboolean is_good_face;
  gdouble quality_score;
  const FaceQualityMetrics *metrics;
  const gchar *face_image_base64;
} FaceContext;

// Global detection manager instance
static DetectionManager *detection_manager = NULL;

#endif
