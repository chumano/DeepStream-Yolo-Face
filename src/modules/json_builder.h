#ifndef __JSON_BUILDER_H__
#define __JSON_BUILDER_H__

#include <glib.h>
#include "face_analysis.h"
#include "image_processing.h"
#include "face.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * Build JSON string from face detection context
 */
gchar *build_detection_json(FaceContext *ctx);

/**
 * Build a lightweight JSON string for a generic (non-face) detected object.
 * Suitable for traffic / secondary-inference detections that carry no
 * landmark or face-quality data.
 *
 * @param source_id   source stream index
 * @param frame_num   frame number
 * @param timestamp   NTP timestamp in seconds
 * @param object_id   tracking object id
 * @param class_id    class index
 * @param label       class label string (may be NULL)
 * @param confidence  detection score
 * @param left/top/width/height  bounding box in pixels
 * @param gie_id      unique_component_id of the inference engine
 * @return newly-allocated JSON string; caller must g_free()
 */
gchar *build_generic_object_json(guint        source_id,
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
                                 guint        gie_id);

/**
 * Build a frame-level JSON string containing all detected objects.
 *
 * @param source_id        source stream index
 * @param frame_num        frame number
 * @param timestamp        NTP timestamp in seconds
 * @param frame_image_path saved frame JPEG path (may be NULL)
 * @param object_jsons     NULL-terminated array of per-object JSON strings
 *                         (each produced by build_detection_json)
 * @param num_objects      number of entries in object_jsons
 * @return newly-allocated JSON string; caller must g_free()
 */
gchar *build_frame_json(guint        source_id,
                        guint        frame_num,
                        gdouble      timestamp,
                        const gchar *frame_image_path,
                        gchar      **object_jsons,
                        guint        num_objects);

/**
 * Build a JSON event string for a completed smart-record session.
 *
 * @param source_id    source stream index
 * @param session_id   NvDsSR session identifier
 * @param file_path    full path of the saved recording file
 * @param duration_sec recording duration in seconds
 * @param container    container format (0 = MP4, 1 = MKV)
 * @param width        frame width of the recording
 * @param height       frame height of the recording
 * @param timestamp    wall-clock time when the event fired (seconds)
 * @return newly-allocated JSON string; caller must g_free()
 */
gchar *build_smart_record_event_json(guint        source_id,
                                     guint        session_id,
                                     const gchar *file_path,
                                     gdouble      duration_sec,
                                     guint        container,
                                     guint        width,
                                     guint        height,
                                     gdouble      timestamp,
                                     guint        frame_num,
                                     guint        num_obj);

#ifdef __cplusplus
}
#endif

#endif
