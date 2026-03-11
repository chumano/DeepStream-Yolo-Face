// filepath: d:\Workspace\tris-forks\DeepStream-Yolo-Face\src\modules\json_builder.c
#include "json_builder.h"
#include "config.h"

gchar *
build_detection_json(FaceContext *ctx)
{
  const CropBox* crop_box = ctx->crop_box;
  const CropBox* bbox = ctx->bbox;

  GString *json = g_string_new("{");

  // Core metadata
  g_string_append_printf(json, "\"timestamp\":%.3f,", ctx->frame_timestamp);
  g_string_append_printf(json, "\"source_id\":%u,", ctx->source_id);
  g_string_append_printf(json, "\"object_id\":%lu,", ctx->object_id);
  g_string_append_printf(json, "\"class_id\":%d,", ctx->class_id);
  g_string_append_printf(json, "\"confidence\":%.4f,", ctx->confidence);

  // Frame info
  g_string_append_printf(json, "\"frame_size\":{\"width\":%d,\"height\":%d},",
                         ctx->frame_width, ctx->frame_height);
  g_string_append_printf(json, "\"frame_number\":%u,", ctx->frame_num);

  // Bounding boxes
  g_string_append_printf(json, "\"bbox\":{\"left\":%u,\"top\":%u,\"width\":%u,\"height\":%u},",
                         bbox->left, bbox->top,
                         bbox->width, bbox->height);
  g_string_append_printf(json, "\"crop_bbox\":{\"left\":%u,\"top\":%u,\"width\":%u,\"height\":%u},",
                         crop_box->left, crop_box->top, crop_box->width, crop_box->height);

  // Landmarks
  g_string_append(json, "\"landmarks\":[");
  for (guint i = 0; i < ctx->num_landmarks; i++) {
    g_string_append_printf(json, "{\"x\":%.2f,\"y\":%.2f,\"confidence\":%.4f}%s",
                           ctx->landmarks[i].x, ctx->landmarks[i].y, 
                           ctx->landmarks[i].confidence,
                           (i < ctx->num_landmarks - 1) ? "," : "");
  }
  g_string_append(json, "],");

  // Face quality metrics
  g_string_append(json, "\"face_quality\":{");
  g_string_append_printf(json, "\"is_good_face\":%s,", ctx->is_good_face ? "true" : "false");
  g_string_append_printf(json, "\"quality_score\":%.3f", ctx->quality_score);
  if (ctx->metrics) {
    g_string_append_printf(json, ",\"is_frontal\":%s", ctx->metrics->is_frontal ? "true" : "false");
    g_string_append_printf(json, ",\"visible_landmarks\":%u", ctx->metrics->visible_landmarks);
    g_string_append_printf(json, ",\"total_landmarks\":%u", ctx->metrics->total_landmarks);
    g_string_append_printf(json, ",\"avg_confidence\":%.3f", ctx->metrics->avg_confidence);
    g_string_append_printf(json, ",\"frontal_score\":%.3f", ctx->metrics->frontal_score);
  }
  g_string_append(json, "}");

  // Face image (optional)
  if (ctx->face_image_base64) {
    g_string_append_printf(json, ",\"face_image\":\"%s\"", ctx->face_image_base64);
  }

  if(ctx->frame_image_path) {
    g_string_append_printf(json, ",\"frame_image_path\":\"%s\"", ctx->frame_image_path);
  }

  g_string_append(json, "}");

  return g_string_free(json, FALSE);
}

gchar *
build_generic_object_json(guint        source_id,
                          guint        frame_num,
                          gdouble      timestamp,
                          guint64      object_id,
                          gint         class_id,
                          const gchar *label,
                          gdouble      confidence,
                          guint        left,
                          guint        top,
                          guint        width,
                          guint        height,
                          guint        gie_id)
{
  GString *json = g_string_new("{");

  g_string_append_printf(json, "\"timestamp\":%.3f,", timestamp);
  g_string_append_printf(json, "\"source_id\":%u,", source_id);
  g_string_append_printf(json, "\"frame_number\":%u,", frame_num);
  g_string_append_printf(json, "\"gie_id\":%u,", gie_id);
  g_string_append_printf(json, "\"object_id\":%lu,", object_id);
  g_string_append_printf(json, "\"class_id\":%d,", class_id);
  if (label && label[0])
    g_string_append_printf(json, "\"label\":\"%s\",", label);
  else
    g_string_append_printf(json, "\"label\":null,");
  g_string_append_printf(json, "\"confidence\":%.4f,", confidence);
  g_string_append_printf(json, "\"bbox\":{\"left\":%u,\"top\":%u,\"width\":%u,\"height\":%u}",
                         left, top, width, height);

  g_string_append_c(json, '}');
  return g_string_free(json, FALSE);
}

gchar *
build_frame_json(guint        source_id,
                 guint        frame_num,
                 gdouble      timestamp,
                 const gchar *frame_image_path,
                 gchar      **object_jsons,
                 guint        num_objects)
{
  GString *json = g_string_new("{");

  g_string_append_printf(json, "\"source_id\":%u,", source_id);
  g_string_append_printf(json, "\"frame_num\":%u,", frame_num);
  g_string_append_printf(json, "\"timestamp\":%.3f,", timestamp);
  g_string_append_printf(json, "\"num_objects\":%u,", num_objects);

  if (frame_image_path)
    g_string_append_printf(json, "\"frame_image_path\":\"%s\",", frame_image_path);
  else
    g_string_append(json, "\"frame_image_path\":null,");

  g_string_append(json, "\"objects\":[");
  for (guint i = 0; i < num_objects; i++) {
    if (object_jsons[i]) {
      g_string_append(json, object_jsons[i]);
      if (i < num_objects - 1)
        g_string_append_c(json, ',');
    }
  }
  g_string_append(json, "]}");

  return g_string_free(json, FALSE);
}

gchar *
build_smart_record_event_json(guint        source_id,
                              guint        session_id,
                              const gchar *file_path,
                              gdouble      duration_sec,
                              guint        container,
                              guint        width,
                              guint        height,
                              gdouble      timestamp,
                              guint        frame_num,
                              guint        num_obj)
{
  GString *json = g_string_new("{");

  g_string_append_printf(json, "\"event\":\"smart_record_done\",");
  g_string_append_printf(json, "\"timestamp\":%.3f,", timestamp);
  g_string_append_printf(json, "\"source_id\":%u,", source_id);
  g_string_append_printf(json, "\"session_id\":%u,", session_id);
  if (file_path)
    g_string_append_printf(json, "\"file\":\"%s\",", file_path);
  else
    g_string_append(json, "\"file\":null,");
  g_string_append_printf(json, "\"duration_sec\":%.3f,", duration_sec);
  g_string_append_printf(json, "\"container\":%u,", container);
  g_string_append_printf(json, "\"width\":%u,", width);
  g_string_append_printf(json, "\"height\":%u,", height);
  g_string_append_printf(json, "\"frame_num\":%u,", frame_num);
  g_string_append_printf(json, "\"num_obj\":%u", num_obj);

  g_string_append_c(json, '}');
  return g_string_free(json, FALSE);
}