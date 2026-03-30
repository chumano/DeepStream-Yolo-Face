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

  /* Display branch (NULL when app_config.display.disabled == TRUE).
   * When app_config.display.nvsink_disabled == TRUE, nvsink is a fakesink. */
  GstElement *queue_display;
  GstElement *nvosd;
  GstElement *nvsink;

  /* Application-sink branch */
  GstElement *queue_app;
  GstElement *appsink;

  /* RTSP sink branch (NULL when app_config.rtsp_sink.enabled == FALSE)
   * Topology: queue_rtsp → nvvidconv_rtsp → capsfilter_rtsp (NV12)
   *           → encoder_rtsp (nvv4l2h264enc / nvv4l2h265enc)
   *           → parse_rtsp   (h264parse / h265parse)
   *           → rtspclientsink (pushes to external RTSP server) */
  GstElement *queue_rtsp;
  GstElement *nvvidconv_rtsp;
  GstElement *capsfilter_rtsp;
  GstElement *encoder_rtsp;
  GstElement *parse_rtsp;
  GstElement *rtspclientsink;

  /* Bus watch id — kept for g_source_remove() during cleanup */
  guint bus_watch_id;

  /* Performance measurement resources */
  NvDsAppPerfStructInt *perf_struct;
  GstPad               *perf_pad; /**< pad used by enable_perf_measurement() */

  /**
   * Per-source raw frame capture branch.
   *
   * When app_config.frame_save.enabled is TRUE, each source's decoded video
   * is routed through a tee *before* nvstreammux so that original-resolution
   * frames (without letterbox or OSD overlays) can be captured.
   *
   * Topology per source N:
   *   [uridecodebin] ─► src_tees[N] ─src_0─► src_tee_mux_queues[N] ─► nvstreammux  (inference path)
   *                                  └─src_1─► src_tee_sink_queues[N]
   *                                            ─► src_nvvidconvs[N]
   *                                            ─► src_capsfilters[N] (RGBA)
   *                                            ─► src_appsinks[N]
   *
   * All arrays have length num_sources.  When frame saving is disabled every
   * entry is NULL and the tee is omitted (direct uridecodebin→nvstreammux link).
   */
  guint       num_sources;
  GstElement **src_tees;             /**< per-source tee; NULL entries when disabled */
  GstElement **src_tee_mux_queues;   /**< queue between tee src_0 and nvstreammux */
  GstElement **src_tee_sink_queues;   /**< queue between tee src_1 and nvvidconv capture chain */
  GstElement **src_nvvidconvs;       /**< per-source nvvideoconvert → RGBA */
  GstElement **src_capsfilters;      /**< per-source capsfilter (RGBA NVMM) */
  GstElement **src_appsinks;         /**< per-source raw-frame appsink */

  /** @private Internal list of heap-allocated SourceBinCtx objects */
  GList *_src_ctxs;

  /**
   * Array of nvurisrcbin elements (one per source).
   * Kept so that the bus-error handler can restart a failing source bin
   * on RTSP disconnect/reconnect events.  Length == num_sources.
   */
  GstElement **src_bins;

  /** GMainLoop stored here so bus_call can quit it on fatal errors. */
  GMainLoop *loop;
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
                                 GCallback        sr_done_callback,
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
