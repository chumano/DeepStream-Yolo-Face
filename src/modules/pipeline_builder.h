#ifndef __PIPELINE_BUILDER_H__
#define __PIPELINE_BUILDER_H__

#include <gst/gst.h>
#include "perf.h"
#include "pipeline_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Holds every GStreamer element and handle that belongs to the DeepStream
 * pipeline.  All fields are populated by create_app_pipeline() and are
 * valid until destroy_app_pipeline() is called.
 *
 * Elements that are conditionally created (display branch, nvinfer2) are
 * NULL when disabled.
 */
typedef struct {
  /* Core pipeline object */
  GstElement *pipeline;

  /* Processing chain */
  GstElement *nvstreammux;
  GstElement *nvinfer;
  GstElement *nvinfer2;    /**< NULL when secondary inference is disabled */
  GstElement *nvtracker;
  GstElement *nvvidconv;
  GstElement *capsfilter;
  GstElement *tee;

  /* Display branch (NULL when app_config.display.disabled == TRUE) */
  GstElement *queue_display;
  GstElement *nvosd;
  GstElement *nvsink;

  /* Application-sink branch */
  GstElement *queue_app;
  GstElement *appsink;

  /* Bus watch id — kept for g_source_remove() during cleanup */
  guint bus_watch_id;

  /* Performance measurement resources */
  NvDsAppPerfStructInt *perf_struct;
  GstPad               *perf_pad; /**< pad used by enable_perf_measurement() */
} AppPipeline;


/**
 * Create the full GStreamer DeepStream pipeline.
 *
 * @loop:             GMainLoop used by the bus error / EOS handler.
 * @appsink_callback: Callback to connect to the appsink "new-sample" signal.
 * @monitor:          Optional PipelineMonitor instance for queue monitoring
 *                    (may be NULL).
 *
 * Returns a heap-allocated AppPipeline on success, or NULL on failure.
 * The caller is responsible for freeing it with destroy_app_pipeline().
 *
 * On failure the function has already printed a descriptive error message to
 * stderr; no partially-constructed pipeline is left behind.
 */
AppPipeline *create_app_pipeline(GMainLoop       *loop,
                                 GCallback        appsink_callback,
                                 PipelineMonitor *monitor);

/**
 * Tear down and free all resources owned by @p.
 * Calls gst_object_unref(pipeline), g_source_remove(bus_watch_id), and
 * g_free(perf_struct).  Does nothing when @p is NULL.
 */
void destroy_app_pipeline(AppPipeline *p);

#ifdef __cplusplus
}
#endif

#endif /* __PIPELINE_BUILDER_H__ */
