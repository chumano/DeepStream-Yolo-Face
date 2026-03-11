#ifndef __PIPELINE_DUMP_H__
#define __PIPELINE_DUMP_H__

#include <gst/gst.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Walk every element in @pipeline recursively (via gst_bin_iterate_recurse)
 * and write a JSON file that lists each element's name, GType, and all
 * readable GObject properties with their current runtime values.
 *
 * Output path: {output_dir}/pipeline_elements.json
 *              Falls back to /tmp/pipeline_elements.json when output_dir is NULL.
 *
 * @pipeline    A GstElement that is also a GstBin (e.g. the top-level pipeline).
 * @output_dir  Directory where the JSON file is written.  May be NULL.
 */
void dump_pipeline_elements_to_json(GstElement  *pipeline,
                                    const gchar *output_dir);

#ifdef __cplusplus
}
#endif

#endif /* __PIPELINE_DUMP_H__ */
