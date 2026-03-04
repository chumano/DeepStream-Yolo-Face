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
// Bus callback
// =============================================================================

static gboolean
bus_call(GstBus *bus, GstMessage *message, gpointer user_data)
{
  GMainLoop *loop = (GMainLoop *) user_data;
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
    {
      GST_DEBUG("EOS");
      g_main_loop_quit(loop);
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
      g_free(debug);
      g_error_free(error);
      g_main_loop_quit(loop);
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
  }
}

static void
uridecodebin_pad_added_callback(GstElement *decodebin, GstPad *pad,
                                 gpointer user_data)
{
  GstPad *nvstreammux_sink_pad = (GstPad *) user_data;

  GstCaps *caps = gst_pad_get_current_caps(pad);
  if (!caps)
    caps = gst_pad_query_caps(pad, NULL);

  const GstStructure *str = gst_caps_get_structure(caps, 0);
  const gchar *name = gst_structure_get_name(str);
  GstCapsFeatures *features = gst_caps_get_features(caps, 0);

  if (!strncmp(name, "video", 5)) {
    if (gst_caps_features_contains(features, "memory:NVMM")) {
      if (gst_pad_link(pad, nvstreammux_sink_pad) != GST_PAD_LINK_OK) {
        GST_ERROR("Failed to link source to nvstreammux sink pad");
      }
    } else {
      GST_ERROR("decodebin did not pick NVIDIA decoder plugin");
    }
  }

  gst_caps_unref(caps);
}

static GstElement *
create_uridecodebin(guint stream_id, const gchar *uri, GstElement *nvstreammux)
{
  gchar bin_name[32] = {};
  g_snprintf(bin_name, 32, "source-bin-%04d", stream_id);

  GstElement *uridecodebin = gst_element_factory_make("uridecodebin", bin_name);

  if (g_strrstr(uri, "rtsp://"))
    configure_source_for_ntp_sync(uridecodebin);

  g_object_set(G_OBJECT(uridecodebin), "uri", uri, NULL);

  gchar pad_name[16];
  g_snprintf(pad_name, 16, "sink_%u", stream_id);

  GstPad *nvstreammux_sink_pad = gst_element_get_request_pad(nvstreammux, pad_name);
  if (!nvstreammux_sink_pad) {
    GST_ERROR("Failed to get nvstreammux %s pad", pad_name);
    return NULL;
  }

  g_signal_connect(G_OBJECT(uridecodebin), "pad-added",
                   G_CALLBACK(uridecodebin_pad_added_callback),
                   nvstreammux_sink_pad);
  g_signal_connect(G_OBJECT(uridecodebin), "child-added",
                   G_CALLBACK(uridecodebin_child_added_callback), NULL);

  gst_object_unref(nvstreammux_sink_pad);
  return uridecodebin;
}

// =============================================================================
// Pipeline creation and teardown
// =============================================================================

AppPipeline *
create_app_pipeline(GMainLoop *loop, GCallback appsink_callback,
                    PipelineMonitor *monitor)
{
  AppPipeline *ap = g_new0(AppPipeline, 1);

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
  // Source bins (one per URI)
  for (guint i = 0; i < app_config.source.count; i++) {
    GstElement *uridecodebin = create_uridecodebin(i, app_config.source.uris[i],
                                                    ap->nvstreammux);
    if (!uridecodebin || !gst_bin_add(GST_BIN(ap->pipeline), uridecodebin)) {
      g_printerr("ERROR - Failed to create uridecodebin for source %d\n", i);
      goto fail;
    }
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
    g_signal_connect(ap->appsink, "new-sample", appsink_callback, NULL);

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
    ap->bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
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
    gst_object_unref(ap->pipeline);
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

  g_free(p->perf_struct);
  g_free(p);
}
