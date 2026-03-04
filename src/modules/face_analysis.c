#include "face_analysis.h"
#include "config.h"
#include <math.h>


gboolean
assess_face_quality(Landmark *landmarks, guint num_landmarks, 
                    gboolean *is_good_face, gdouble *quality_score, 
                    FaceQualityMetrics *metrics)
{
  if (!landmarks || num_landmarks < 5) {
    *is_good_face = FALSE;
    *quality_score = 0.0;
    return FALSE;
  }
  
  // Count visible landmarks
  guint visible_count = 0;
  gdouble confidence_sum = 0.0;
  
  for (guint i = 0; i < num_landmarks; i++) {
    if (landmarks[i].confidence >= app_config.face_quality.min_landmark_confidence) {
      visible_count++;
      confidence_sum += landmarks[i].confidence;
    }
  }
  
  if (visible_count < app_config.face_quality.min_visible_landmarks) {
    *is_good_face = FALSE;
    *quality_score = 0.0;
    if (metrics) {
      metrics->is_frontal = FALSE;
      metrics->visible_landmarks = visible_count;
      metrics->total_landmarks = num_landmarks;
      metrics->avg_confidence = 0.0;
      metrics->quality_score = 0.0;
      metrics->frontal_score = 0.0;
    }
    return FALSE;
  }
  
  gdouble avg_confidence = confidence_sum / visible_count;
  
  // Extract key landmarks
  Landmark *left_eye = &landmarks[0];
  Landmark *right_eye = &landmarks[1];
  Landmark *nose = &landmarks[2];
  
  gboolean is_frontal = FALSE;
  gdouble frontal_score = 0.0;
  
  if (left_eye->confidence >= app_config.face_quality.min_landmark_confidence &&
      right_eye->confidence >= app_config.face_quality.min_landmark_confidence &&
      nose->confidence >= app_config.face_quality.min_landmark_confidence) {
    
    gdouble eye_distance = sqrt(pow(right_eye->x - left_eye->x, 2) + 
                                pow(right_eye->y - left_eye->y, 2));
    
    if (eye_distance > 0) {
      gdouble eye_center_x = (left_eye->x + right_eye->x) / 2.0;
      gdouble nose_offset_x = fabs(nose->x - eye_center_x);
      gdouble nose_offset_ratio = nose_offset_x / eye_distance;
      
      frontal_score = MAX(0.0, 1.0 - nose_offset_ratio * 2.0);
      is_frontal = (frontal_score >= app_config.face_quality.min_frontal_score);
    }
  }
  
  *quality_score = (avg_confidence * 0.5) + (frontal_score * 0.5);
  
  if (metrics) {
    metrics->is_frontal = is_frontal;
    metrics->visible_landmarks = visible_count;
    metrics->total_landmarks = num_landmarks;
    metrics->avg_confidence = avg_confidence;
    metrics->quality_score = *quality_score;
    metrics->frontal_score = frontal_score;
  }
  
  *is_good_face = (visible_count >= app_config.face_quality.min_visible_landmarks &&
                   *quality_score >= app_config.face_quality.face_quality_threshold &&
                   is_frontal);
  
  return TRUE;
}

Landmark *
extract_landmarks_from_object(NvDsObjectMeta *obj_meta, guint *num_landmarks_out, guint frame_width, guint frame_height)
{
  if (obj_meta->mask_params.size == 0) {
    *num_landmarks_out = 0;
    GST_DEBUG("Landmark data size is zero");
    return NULL;
  }

  if(!obj_meta->mask_params.data){
    *num_landmarks_out = 0;
    GST_DEBUG("Landmark data pointer is NULL");
    return NULL;
  }

  guint num_joints = obj_meta->mask_params.size / (sizeof(float) * 3);
  if (num_joints == 0) {
    *num_landmarks_out = 0;
    GST_DEBUG("Landmark data size is zero");
    return NULL;
  }
  
  gfloat gain = MIN((gfloat)obj_meta->mask_params.width / frame_width,
                    (gfloat)obj_meta->mask_params.height / frame_height);
  gfloat pad_x = (obj_meta->mask_params.width - frame_width * gain) * 0.5f;
  gfloat pad_y = (obj_meta->mask_params.height - frame_height * gain) * 0.5f;

  // print debug info about landmarks
  GST_DEBUG("Extracting %u landmarks for object_id=%lu (gain=%.3f, pad_x=%.1f, pad_y=%.1f)\n",
        num_joints, obj_meta->object_id, gain, pad_x, pad_y);

  Landmark *landmarks = g_malloc(sizeof(Landmark) * num_joints);

  for (guint i = 0; i < num_joints; i++) {
    landmarks[i].x = (obj_meta->mask_params.data[i * 3 + 0] - pad_x) / gain;
    landmarks[i].y = (obj_meta->mask_params.data[i * 3 + 1] - pad_y) / gain;
    landmarks[i].confidence = obj_meta->mask_params.data[i * 3 + 2];
  }

  *num_landmarks_out = num_joints;
  return landmarks;
}
