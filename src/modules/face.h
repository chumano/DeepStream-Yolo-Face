#ifndef __FACE__H__
#define __FACE__H__

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _Landmark {
  gdouble x;
  gdouble y;
  gdouble confidence;
} Landmark;

typedef struct _FaceQualityMetrics {
  gboolean is_frontal;
  guint visible_landmarks;
  guint total_landmarks;
  gdouble avg_confidence;
  gdouble quality_score;
  gdouble frontal_score;
} FaceQualityMetrics;

typedef struct {
  guint left;
  guint top;
  guint width;
  guint height;
} CropBox;

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

#ifdef __cplusplus
}
#endif

#endif