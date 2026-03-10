#include "pipeline_builder.h"

#include <nvdsgstutils.h>
#include <nvbufsurface.h>
#include "gstnvdsmeta.h"

#include "config.h"
#include "osd_probe.h"
#include "perf.h"

// Reuse the debug category defined in deepstream.cpp
GST_DEBUG_CATEGORY_EXTERN(deepstream_debug_category);
#define GST_CAT_DEFAULT deepstream_debug_category

// =============================================================================
// Bus callback helpers
// =============================================================================

/**
 * Walk the element hierarchy from @element upward until we reach a direct
 * child of @pipeline whose name starts with "source-bin-".
 * Returns a new GstElement reference on success (caller must gst_object_unref)
 * or NULL when not found.
 */
static GstElement *
find_toplevel_source_bin(GstElement *element, GstElement *pipeline)
{
  GstObject *obj = gst_object_ref(GST_OBJECT(element));
  GstElement *found = NULL;

  while (obj) {
    GstObject *parent = gst_object_get_parent(obj);
    if (!parent) {
      gst_object_unref(obj);
      break;
    }
    if (GST_ELEMENT(parent) == pipeline) {
      if (g_str_has_prefix(GST_OBJECT_NAME(obj), "source-bin-"))
        found = GST_ELEMENT(gst_object_ref(obj));
      gst_object_unref(obj);
      gst_object_unref(parent);
      break;
    }
    gst_object_unref(obj);
    obj = parent;
  }

  return found;
}

// =============================================================================
// Bus callback
// =============================================================================

static gboolean
bus_call(GstBus *bus, GstMessage *message, gpointer user_data)
{
  AppPipeline *ap = (AppPipeline *) user_data;
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
    {
      GST_DEBUG("EOS");
      g_main_loop_quit(ap->loop);
      break;
    }
    case GST_MESSAGE_WARNING:
    {
      gchar *debug;
      GError *error;
      gst_message_parse_warning(message, &error, &debug);
      GST_WARNING("%s - %s", error->message, debug);
      g_free(debug);
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_ERROR:
    {
      gchar *debug;
      GError *error;
      gst_message_parse_error(message, &error, &debug);
      GST_ERROR("%s - %s", error->message, debug);

      /*
       * For GStreamer resource errors (typically RTSP connection drops) try to
       * restart only the affected source bin so the rest of the pipeline keeps
       * running.  Any other error domain is treated as fatal.
       */
      if (error->domain == GST_RESOURCE_ERROR && ap && ap->pipeline) {
        GstElement *src_bin = find_toplevel_source_bin(
            GST_ELEMENT(GST_MESSAGE_SRC(message)), ap->pipeline);
        if (src_bin) {
          GST_WARNING("RTSP resource error on %s — restarting source bin",
                      GST_ELEMENT_NAME(src_bin));
          gst_element_set_state(src_bin, GST_STATE_NULL);
          gst_element_set_state(src_bin, GST_STATE_PLAYING);
          gst_object_unref(src_bin);
          g_free(debug);
          g_error_free(error);
          break;
        }
      }

      g_free(debug);
      g_error_free(error);
      /* Non-recoverable error — quit the main loop */
      if (ap && ap->loop)
        g_main_loop_quit(ap->loop);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

// =============================================================================
// Source bin helpers
// =============================================================================

/**
 * Context passed to the nvurisrcbin "pad-added" signal.
 *
 * When a per-source raw capture branch is present (frame_save enabled) the
 * decoded NVMM pad is linked to the tee's static sink pad.  Otherwise it is
 * linked directly to the nvstreammux request pad stored in mux_sink_pad.
 */
typedef struct {
  GstElement *tee;           /**< non-NULL when raw capture branch exists */
  GstPad     *mux_sink_pad;  /**< non-NULL when tee is NULL (direct mode) */
} SourceBinCtx;
static void
uridecodebin_child_added_callback(GstChildProxy *child_proxy, GObject *object,
                                   gchar *name, gpointer user_data)
{
  if (g_strrstr(name, "decodebin")) {
    g_signal_connect(object, "child-added",
                     G_CALLBACK(uridecodebin_child_added_callback), user_data);
  } else if (g_strrstr(name, "nvv4l2decoder")) {
    g_object_set(object, "drop-frame-interval", 0,
                 "num-extra-surfaces", 1, "qos", 0, NULL);
    if (app_config.jetson) {
      g_object_set(object, "enable-max-performance", 1, NULL);
    } else {
      g_object_set(object, "cudadec-memtype", 0,
                   "gpu-id", app_config.gpu_id, NULL);
    }
  } else if (g_strrstr(name, "rtspsrc")) {
    /*
     * Tune the rtspsrc element for resilient RTSP reconnection:
     *   do-rtsp-keep-alive FALSE — suppress periodic keep-alive RTSP OPTIONS
     *                              requests; a dead connection shouldn't
     *                              trigger a flood of failed sends.
     *   timeout            5 s   — give up waiting for a server response
     *                              within 5 s so the reconnect cycle fires
     *                              promptly.
     *   tcp-timeout        5 s   — same for the underlying TCP socket.
     *   retry              10    — rtspsrc-level retry count before escalating
     *                              to a bus ERROR (nvurisrcbin will still
     *                              attempt higher-level reconnects).
     */
    // g_object_set(object,
    //              "do-rtsp-keep-alive", FALSE,
    //              "timeout",           (guint64) 5000000,  /* µs */
    //              "tcp-timeout",       (guint64) 5000000,  /* µs */
    //              "retry",             10,
    //              NULL);
  }
}

static void
nvurisrcbin_pad_added_callback(GstElement *srcbin, GstPad *pad,
                                gpointer user_data)
{
  SourceBinCtx *ctx = (SourceBinCtx *) user_data;

  GstCaps *caps = gst_pad_get_current_caps(pad);
  if (!caps)
    caps = gst_pad_query_caps(pad, NULL);

  const GstStructure *str = gst_caps_get_structure(caps, 0);
  const gchar *name = gst_structure_get_name(str);
  GstCapsFeatures *features = gst_caps_get_features(caps, 0);

  if (!strncmp(name, "video", 5)) {
    if (gst_caps_features_contains(features, "memory:NVMM")) {
      GstPad *sink_pad;
      if (ctx->tee) {
        /* Route through per-source tee (raw capture branch) */
        sink_pad = gst_element_get_static_pad(ctx->tee, "sink");
      } else {
        /* Direct link to nvstreammux */
        sink_pad = ctx->mux_sink_pad;
        gst_object_ref(sink_pad);  /* match unref below */
      }
      if (gst_pad_link(pad, sink_pad) != GST_PAD_LINK_OK) {
        GST_ERROR("Failed to link source to %s pad",
                  ctx->tee ? "src_tee" : "nvstreammux");
      }
      gst_object_unref(sink_pad);
    } else {
      GST_ERROR("nvurisrcbin did not produce NVMM output");
    }
  }

  gst_caps_unref(caps);
}

static GstElement *
create_nvurisrcbin(guint stream_id, const gchar *uri, SourceBinCtx *ctx)
{
  gchar bin_name[32] = {};
  g_snprintf(bin_name, 32, "source-bin-%04d", stream_id);

  GstElement *nvurisrcbin = gst_element_factory_make("nvurisrcbin", bin_name);
  if (!nvurisrcbin) {
    g_printerr("ERROR - Failed to create nvurisrcbin (is DeepStream installed?)\n");
    return NULL;
  }

  g_object_set(G_OBJECT(nvurisrcbin),
               "uri",             uri,
               "gpu-id",          app_config.gpu_id,
               "cudadec-memtype", 0,
               NULL);

  if (g_strrstr(uri, "rtsp://")) {
    //configure_source_for_ntp_sync(nvurisrcbin); // Source element type nvurisrcbin is not supported

    // set reconection properties for RTSP sources
    g_object_set(G_OBJECT(nvurisrcbin),
                 "rtsp-reconnect-interval", 15, // Reconnect every 15 seconds
                 "rtsp-reconnect-attempts", -1, // Retry indefinitely
                  NULL);

    /* ── Smart Record ── */
    if (app_config.smart_record.enabled && app_config.smart_record.dir) {
      g_object_set(G_OBJECT(nvurisrcbin),
                   "smart-record",           1,
                   "smart-rec-dir-path",      app_config.smart_record.dir,
                   "smart-rec-cache",         app_config.smart_record.cache_size_sec,
                   "smart-rec-default-duration", app_config.smart_record.default_duration_sec,
                   "smart-rec-container",     app_config.smart_record.container,
                   NULL);
      if (app_config.smart_record.file_prefix) {
        g_object_set(G_OBJECT(nvurisrcbin),
                     "smart-rec-file-prefix", app_config.smart_record.file_prefix,
                     NULL);
      }
      GST_INFO("pipeline_builder: smart-record enabled for source %u (dir=%s)",
               stream_id, app_config.smart_record.dir);
    }
  }

  g_signal_connect(G_OBJECT(nvurisrcbin), "pad-added",
                   G_CALLBACK(nvurisrcbin_pad_added_callback), ctx);
  g_signal_connect(G_OBJECT(nvurisrcbin), "child-added",
                   G_CALLBACK(uridecodebin_child_added_callback), NULL);

  return nvurisrcbin;
}

// =============================================================================
// Pipeline creation and teardown
// =============================================================================

AppPipeline *
create_app_pipeline(GMainLoop *loop, GCallback appsink_callback,
                    PipelineMonitor *monitor)
{
  AppPipeline *ap = g_new0(AppPipeline, 1);
  ap->loop = loop;

  // ---------------------------------------------------------------------------
  // Pipeline
  ap->pipeline = gst_pipeline_new("deepstream");
  if (!ap->pipeline) {
    g_printerr("ERROR - Failed to create pipeline\n");
    goto fail;
  }

  // ---------------------------------------------------------------------------
  // nvstreammux
  ap->nvstreammux = gst_element_factory_make("nvstreammux", "nvstreammux");
  if (!ap->nvstreammux || !gst_bin_add(GST_BIN(ap->pipeline), ap->nvstreammux)) {
    g_printerr("ERROR - Failed to create nvstreammux\n");
    goto fail;
  }

  // ---------------------------------------------------------------------------
  // Source bins (one per URI) + optional per-source raw capture branch
  ap->num_sources    = app_config.source.count;
  ap->src_bins       = g_new0(GstElement *, ap->num_sources);
  ap->src_tees       = g_new0(GstElement *, ap->num_sources);
  ap->src_nvvidconvs = g_new0(GstElement *, ap->num_sources);
  ap->src_capsfilters= g_new0(GstElement *, ap->num_sources);
  ap->src_queues     = g_new0(GstElement *, ap->num_sources);
  ap->src_appsinks   = g_new0(GstElement *, ap->num_sources);

  for (guint i = 0; i < app_config.source.count; i++) {
    /* --- Request per-source nvstreammux sink pad --- */
    gchar pad_name[16];
    g_snprintf(pad_name, 16, "sink_%u", i);
    GstPad *mux_sink_pad = gst_element_get_request_pad(ap->nvstreammux, pad_name);
    if (!mux_sink_pad) {
      g_printerr("ERROR - Failed to get nvstreammux %s pad\n", pad_name);
      goto fail;
    }

    /* --- Build SourceBinCtx (lives until destroy_app_pipeline) --- */
    SourceBinCtx *ctx = g_new0(SourceBinCtx, 1);
    ctx->mux_sink_pad = mux_sink_pad;  /* always track; cleared once consumed */
    ap->_src_ctxs = g_list_append(ap->_src_ctxs, ctx);

    if (app_config.frame_save.enabled) {
      /* ── Per-source tee ── */
      gchar elem_name[48];
      g_snprintf(elem_name, 48, "src_tee_%u", i);
      ap->src_tees[i] = gst_element_factory_make("tee", elem_name);
      if (!ap->src_tees[i] || !gst_bin_add(GST_BIN(ap->pipeline), ap->src_tees[i])) {
        g_printerr("ERROR - Failed to create src_tee_%u\n", i);
        goto fail;
      }
      g_object_set(G_OBJECT(ap->src_tees[i]), "allow-not-linked", FALSE, NULL);

      /* ── Raw capture chain: nvvidconv → capsfilter(RGBA) → queue → appsink ── */
      g_snprintf(elem_name, 48, "src_nvvidconv_%u", i);
      ap->src_nvvidconvs[i] = gst_element_factory_make("nvvideoconvert", elem_name);
      g_snprintf(elem_name, 48, "src_capsfilter_%u", i);
      ap->src_capsfilters[i] = gst_element_factory_make("capsfilter", elem_name);
      g_snprintf(elem_name, 48, "src_queue_%u", i);
      ap->src_queues[i] = gst_element_factory_make("queue", elem_name);
      g_snprintf(elem_name, 48, "src_appsink_%u", i);
      ap->src_appsinks[i] = gst_element_factory_make("appsink", elem_name);

      if (!ap->src_nvvidconvs[i] || !ap->src_capsfilters[i] ||
          !ap->src_queues[i]     || !ap->src_appsinks[i]) {
        g_printerr("ERROR - Failed to create raw capture elements for source %u\n", i);
        goto fail;
      }
      gst_bin_add_many(GST_BIN(ap->pipeline),
                       ap->src_nvvidconvs[i], ap->src_capsfilters[i],
                       ap->src_queues[i],     ap->src_appsinks[i], NULL);

      /* Configure raw capture chain */
      GstCaps *rgba_caps = gst_caps_from_string(
          "video/x-raw(memory:NVMM), format=RGBA");
      g_object_set(G_OBJECT(ap->src_capsfilters[i]), "caps", rgba_caps, NULL);
      gst_caps_unref(rgba_caps);

      /* Leaky downstream queue — drop oldest when full */
      g_object_set(G_OBJECT(ap->src_queues[i]),
                   "max-size-buffers", 5, "leaky", 2, NULL);

      /* Non-synced appsink, drop when full */
      g_object_set(G_OBJECT(ap->src_appsinks[i]),
                   "emit-signals", TRUE,
                   "sync",         FALSE,
                   "max-buffers",  5,
                   "drop",        TRUE, NULL);

      if (!app_config.jetson) {
        g_object_set(G_OBJECT(ap->src_nvvidconvs[i]),
                     "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE,
                     "gpu_id",           app_config.gpu_id, NULL);
      }

      /* Link: tee src_0 → nvstreammux sink */
      {
        GstPad *tee_mux_src = gst_element_get_request_pad(
            ap->src_tees[i], "src_%u");
        if (gst_pad_link(tee_mux_src, mux_sink_pad) != GST_PAD_LINK_OK) {
          g_printerr("ERROR - Failed to link tee to nvstreammux for source %u\n", i);
          gst_object_unref(tee_mux_src);
          gst_object_unref(mux_sink_pad);
          goto fail;
        }
        gst_object_unref(tee_mux_src);
        gst_object_unref(mux_sink_pad);  /* owned by mux; drop our ref */
        ctx->mux_sink_pad = NULL;         /* consumed — don't double-unref in cleanup */
      }

      /* Link: tee src_1 → nvvidconv */
      {
        GstPad *tee_cap_src = gst_element_get_request_pad(
            ap->src_tees[i], "src_%u");
        GstPad *nvvc_sink = gst_element_get_static_pad(
            ap->src_nvvidconvs[i], "sink");
        if (gst_pad_link(tee_cap_src, nvvc_sink) != GST_PAD_LINK_OK) {
          g_printerr("ERROR - Failed to link tee to nvvidconv for source %u\n", i);
          gst_object_unref(tee_cap_src);
          gst_object_unref(nvvc_sink);
          goto fail;
        }
        gst_object_unref(tee_cap_src);
        gst_object_unref(nvvc_sink);
      }

      if (!gst_element_link_many(ap->src_nvvidconvs[i], ap->src_capsfilters[i],
                                 ap->src_queues[i], ap->src_appsinks[i], NULL)) {
        g_printerr("ERROR - Failed to link raw capture chain for source %u\n", i);
        goto fail;
      }

      ctx->tee          = ap->src_tees[i];
      /* mux_sink_pad already consumed and cleared above */

    } else {
      /* Direct link: nvurisrcbin decoded pad → nvstreammux (pad-added fires) */
      ctx->tee = NULL;
      /* ctx->mux_sink_pad already set above; pad-added callback will use it */
    }

    /* --- Create nvurisrcbin with SourceBinCtx as pad-added context --- */
    GstElement *nvurisrcbin = create_nvurisrcbin(
        i, app_config.source.uris[i], ctx);
    if (!nvurisrcbin || !gst_bin_add(GST_BIN(ap->pipeline), nvurisrcbin)) {
      g_printerr("ERROR - Failed to create nvurisrcbin for source %d\n", i);
      goto fail;
    }
    ap->src_bins[i] = nvurisrcbin;
  }

  // ---------------------------------------------------------------------------
  // Primary inference element (nvinfer or nvinferserver)
  ap->nvinfer = gst_element_factory_make(
      app_config.infer.use_triton ? "nvinferserver" : "nvinfer",
      app_config.infer.use_triton ? "nvinferserver" : "nvinfer");
  if (!ap->nvinfer || !gst_bin_add(GST_BIN(ap->pipeline), ap->nvinfer)) {
    g_printerr("ERROR - Failed to create %s\n",
               app_config.infer.use_triton ? "nvinferserver" : "nvinfer");
    goto fail;
  }

  // ---------------------------------------------------------------------------
  // Secondary nvinferserver (Triton) — only when infer2.config_file is set
  if (app_config.infer2.config_file) {
    ap->nvinfer2 = gst_element_factory_make("nvinferserver", "nvinferserver2");
    if (!ap->nvinfer2 || !gst_bin_add(GST_BIN(ap->pipeline), ap->nvinfer2)) {
      g_printerr("ERROR - Failed to create nvinferserver2\n");
      goto fail;
    }
  }

  // ---------------------------------------------------------------------------
  // Tracker
  ap->nvtracker = gst_element_factory_make("nvtracker", "nvtracker");
  if (!ap->nvtracker || !gst_bin_add(GST_BIN(ap->pipeline), ap->nvtracker)) {
    g_printerr("ERROR - Failed to create nvtracker\n");
    goto fail;
  }

  // ---------------------------------------------------------------------------
  // Video converter and caps filter
  ap->nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvidconv");
  if (!ap->nvvidconv || !gst_bin_add(GST_BIN(ap->pipeline), ap->nvvidconv)) {
    g_printerr("ERROR - Failed to create nvvideoconvert\n");
    goto fail;
  }

  ap->capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
  if (!ap->capsfilter || !gst_bin_add(GST_BIN(ap->pipeline), ap->capsfilter)) {
    g_printerr("ERROR - Failed to create capsfilter\n");
    goto fail;
  }

  // ---------------------------------------------------------------------------
  // Tee — splits stream to display branch and appsink branch
  ap->tee = gst_element_factory_make("tee", "tee");
  if (!ap->tee || !gst_bin_add(GST_BIN(ap->pipeline), ap->tee)) {
    g_printerr("ERROR - Failed to create tee\n");
    goto fail;
  }

  // ---------------------------------------------------------------------------
  // Display branch (conditional)
  if (!app_config.display.disabled) {
    // queue_display
    ap->queue_display = gst_element_factory_make("queue", "queue_display");
    if (!ap->queue_display || !gst_bin_add(GST_BIN(ap->pipeline), ap->queue_display)) {
      g_printerr("ERROR - Failed to create queue_display\n");
      goto fail;
    }
    g_object_set(G_OBJECT(ap->queue_display),
                 "max-size-buffers", app_config.queue.max_size_buffers,
                 "leaky", app_config.queue.leaky,
                 NULL);
    if (monitor)
      pipeline_monitor_add_queue(monitor, "queue_display", ap->queue_display);

    // nvosd
    ap->nvosd = gst_element_factory_make("nvdsosd", "nvdsosd");
    if (!ap->nvosd || !gst_bin_add(GST_BIN(ap->pipeline), ap->nvosd)) {
      g_printerr("ERROR - Failed to create nvdsosd\n");
      goto fail;
    }
    g_object_set(G_OBJECT(ap->nvosd),
                 "process-mode", app_config.osd.process_mode,
                 "qos", (gint) app_config.osd.qos,
                 NULL);
    if (!app_config.jetson)
      g_object_set(G_OBJECT(ap->nvosd), "gpu_id", app_config.gpu_id, NULL);

    // display sink
    if (app_config.jetson) {
      ap->nvsink = gst_element_factory_make("nv3dsink", "nv3dsink");
      if (!ap->nvsink || !gst_bin_add(GST_BIN(ap->pipeline), ap->nvsink)) {
        g_printerr("ERROR - Failed to create nv3dsink\n");
        goto fail;
      }
    } else {
      ap->nvsink = gst_element_factory_make("nveglglessink", "nveglglessink");
      if (!ap->nvsink || !gst_bin_add(GST_BIN(ap->pipeline), ap->nvsink)) {
        g_printerr("ERROR - Failed to create nveglglessink\n");
        goto fail;
      }
    }
    g_object_set(G_OBJECT(ap->nvsink),
                 "async", (gint) app_config.display.async_sink,
                 "sync", (gint) app_config.display.sync,
                 "qos", (gint) app_config.display.qos,
                 NULL);
    g_object_set(G_OBJECT(ap->nvsink),
                 "window-width",  app_config.display.window_width,
                 "window-height", app_config.display.window_height,
                 NULL);
  }

  // ---------------------------------------------------------------------------
  // Application sink branch
  ap->queue_app = gst_element_factory_make("queue", "queue_app");
  if (!ap->queue_app || !gst_bin_add(GST_BIN(ap->pipeline), ap->queue_app)) {
    g_printerr("ERROR - Failed to create queue_app\n");
    goto fail;
  }
  g_object_set(G_OBJECT(ap->queue_app),
               "max-size-buffers", app_config.queue.max_size_buffers,
               "leaky", app_config.queue.leaky,
               NULL);
  if (monitor)
    pipeline_monitor_add_queue(monitor, "queue_app", ap->queue_app);

  ap->appsink = gst_element_factory_make("appsink", "appsink");
  if (!ap->appsink || !gst_bin_add(GST_BIN(ap->pipeline), ap->appsink)) {
    g_printerr("ERROR - Failed to create appsink\n");
    goto fail;
  }
  g_object_set(G_OBJECT(ap->appsink),
               "emit-signals", TRUE,
               "sync",         (gint) app_config.appsink.sync,
               "max-buffers",  app_config.appsink.max_buffers,
               "drop",         (gint) app_config.appsink.drop,
               NULL);
  if (appsink_callback)
    g_signal_connect(ap->appsink, "new-sample", appsink_callback, ap);

  // ---------------------------------------------------------------------------
  // Configure elements
  GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=RGBA");
  g_object_set(G_OBJECT(ap->capsfilter), "caps", caps, NULL);
  gst_caps_unref(caps);

  g_object_set(G_OBJECT(ap->nvstreammux),
               "batch-size",           app_config.streammux.batch_size,
               "enable-padding",       app_config.streammux.enable_padding,
               "batched-push-timeout", app_config.streammux.batched_push_timeout,
               "width",                app_config.streammux.width,
               "height",               app_config.streammux.height,
               "live-source",          1,
               NULL);

  g_object_set(G_OBJECT(ap->nvinfer),
               "config-file-path", app_config.infer.config_file,
               "qos",              (gint) app_config.infer.qos,
               NULL);

  if (ap->nvinfer2)
    g_object_set(G_OBJECT(ap->nvinfer2),
                 "config-file-path", app_config.infer2.config_file,
                 "qos",              (gint) app_config.infer2.qos,
                 NULL);

  g_object_set(G_OBJECT(ap->nvtracker),
               "tracker-width",       app_config.tracker.width,
               "tracker-height",      app_config.tracker.height,
               "ll-lib-file",         app_config.tracker.ll_lib_file,
               "ll-config-file",      app_config.tracker.ll_config_file,
               "gpu-id",              app_config.gpu_id,
               "display-tracking-id", (gint) app_config.tracker.display_tracking_id,
               NULL);

  // Override live-source when all inputs are files
  gboolean all_file_sources = TRUE;
  for (guint i = 0; i < app_config.source.count; i++) {
    if (!g_strrstr(app_config.source.uris[i], "file://")) {
      all_file_sources = FALSE;
      break;
    }
  }
  if (all_file_sources) {
    g_print("All sources are file-based. Setting live-source to 0.\n");
    g_object_set(G_OBJECT(ap->nvstreammux), "live-source", 0, NULL);
  }

  if (!app_config.jetson) {
    g_object_set(G_OBJECT(ap->nvstreammux),
                 "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE,
                 "gpu_id", app_config.gpu_id,
                 NULL);
    if (!app_config.infer.use_triton)
      g_object_set(G_OBJECT(ap->nvinfer), "gpu_id", app_config.gpu_id, NULL);
    g_object_set(G_OBJECT(ap->nvvidconv),
                 "nvbuf-memory-type", NVBUF_MEM_CUDA_DEVICE,
                 "gpu_id", app_config.gpu_id,
                 NULL);
  }

  // ---------------------------------------------------------------------------
  // Link elements
  //   nvstreammux -> nvinfer -> [nvinfer2] -> nvtracker -> nvvidconv -> capsfilter -> tee
  if (ap->nvinfer2) {
    if (!gst_element_link_many(ap->nvstreammux, ap->nvinfer, ap->nvinfer2,
                               ap->nvtracker, ap->nvvidconv, ap->capsfilter,
                               ap->tee, NULL)) {
      g_printerr("ERROR - Failed to link pipeline elements (with nvinfer2) to tee\n");
      goto fail;
    }
  } else {
    if (!gst_element_link_many(ap->nvstreammux, ap->nvinfer, ap->nvtracker,
                               ap->nvvidconv, ap->capsfilter, ap->tee, NULL)) {
      g_printerr("ERROR - Failed to link pipeline elements to tee\n");
      goto fail;
    }
  }

  //   tee -> queue_app -> appsink
  if (!gst_element_link_many(ap->tee, ap->queue_app, ap->appsink, NULL)) {
    g_printerr("ERROR - Failed to link tee to appsink\n");
    goto fail;
  }

  //   tee -> display branch  (or fakesink when display is disabled)
  if (!app_config.display.disabled) {
    if (!gst_element_link_many(ap->tee, ap->queue_display, ap->nvosd,
                               ap->nvsink, NULL)) {
      g_printerr("ERROR - Failed to link tee to display sink\n");
      goto fail;
    }
  } else {
    GstElement *fakesink = gst_element_factory_make("fakesink", "fakesink");
    if (!fakesink || !gst_bin_add(GST_BIN(ap->pipeline), fakesink)) {
      g_printerr("ERROR - Failed to create fakesink\n");
      goto fail;
    }
    g_object_set(G_OBJECT(fakesink), "async", FALSE, "sync", FALSE, NULL);
    if (!gst_element_link_many(ap->tee, fakesink, NULL)) {
      g_printerr("ERROR - Failed to link tee to fakesink\n");
      goto fail;
    }
  }

  // ---------------------------------------------------------------------------
  // Bus watch
  {
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(ap->pipeline));
    ap->bus_watch_id = gst_bus_add_watch(bus, bus_call, ap);
    gst_object_unref(bus);
  }

  // ---------------------------------------------------------------------------
  // Performance measurement and OSD sink-pad probe
  ap->perf_struct = (NvDsAppPerfStructInt *) g_malloc0(sizeof(NvDsAppPerfStructInt));

  if (!app_config.display.disabled) {
    GstPad *nvosd_sink_pad = gst_element_get_static_pad(ap->nvosd, "sink");
    if (!nvosd_sink_pad) {
      g_printerr("ERROR - Failed to get nvosd sink pad\n");
      goto fail;
    }
    gst_pad_add_probe(nvosd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                      nvosd_sink_pad_buffer_probe, NULL, NULL);
    ap->perf_pad = nvosd_sink_pad;  /* take ownership */
  } else {
    ap->perf_pad = gst_element_get_static_pad(ap->tee, "sink");
    if (!ap->perf_pad) {
      g_printerr("ERROR - Failed to get tee sink pad\n");
      goto fail;
    }
  }

  enable_perf_measurement(ap->perf_struct, ap->perf_pad,
                           app_config.source.count,
                           app_config.perf_measurement_interval_sec,
                           0, perf_cb);

  return ap;

fail:
  if (ap->pipeline)
    gst_object_unref(ap->pipeline);  /* unrefs all child elements */

  for (GList *l = ap->_src_ctxs; l != NULL; l = l->next) {
    SourceBinCtx *ctx = (SourceBinCtx *)l->data;
    if (ctx && ctx->mux_sink_pad)
      gst_object_unref(ctx->mux_sink_pad);
    g_free(ctx);
  }
  g_list_free(ap->_src_ctxs);

  g_free(ap->src_bins);
  g_free(ap->src_tees);
  g_free(ap->src_nvvidconvs);
  g_free(ap->src_capsfilters);
  g_free(ap->src_queues);
  g_free(ap->src_appsinks);
  g_free(ap->perf_struct);
  g_free(ap);
  return NULL;
}

void
destroy_app_pipeline(AppPipeline *p)
{
  if (!p)
    return;

  if (p->pipeline)
    gst_object_unref(GST_OBJECT(p->pipeline));

  if (p->bus_watch_id)
    g_source_remove(p->bus_watch_id);

  /* Free SourceBinCtx list and any unreleased pad refs */
  for (GList *l = p->_src_ctxs; l != NULL; l = l->next) {
    SourceBinCtx *ctx = (SourceBinCtx *)l->data;
    if (ctx && ctx->mux_sink_pad)
      gst_object_unref(ctx->mux_sink_pad);
    g_free(ctx);
  }
  g_list_free(p->_src_ctxs);

  /* Free per-source element pointer arrays (elements are owned by pipeline) */
  g_free(p->src_bins);
  g_free(p->src_tees);
  g_free(p->src_nvvidconvs);
  g_free(p->src_capsfilters);
  g_free(p->src_queues);
  g_free(p->src_appsinks);

  g_free(p->perf_struct);
  g_free(p);
}
