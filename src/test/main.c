/*
 * DeepStream Pipeline with nvds_obj_encode frame saving
 *
 * Pipeline:
 *   filesrc -> qtdemux -> h264parse -> nvv4l2decoder -> nvstreammux ->
 *   nvinfer -> nvdsosd -> nvvideoconvert -> appsink
 *
 * Uses nvds_obj_encode to save detected object crops as JPEG files.
 *
 * Build:  make
 * Run:    ./deepstream_app <input_mp4_file>
 */

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <cuda_runtime_api.h>

#include "gstnvdsmeta.h"
#include "nvbufsurface.h"
#include "nvds_obj_encode.h"
#include "nvdsmeta.h"

// Program run in docker container :  docker exec -it deepstream-yolo-face-dev /bin/bash
// /app/DeepStream-Yolo-Face/test

/*
typedef struct _NvDsObjEncUsrArgs
{
  bool saveImg;
  bool attachUsrMeta;
  bool scaleImg;
  int scaledWidth;

  int scaledHeight;

  char fileNameImg[FILE_NAME_SIZE];
  int objNum;
  int quality;
  bool isFrame;
  bool calcEncodeTime;
} NvDsObjEncUsrArgs;
*/

/* ── tunables ─────────────────────────────────────────────────── */
#define MUXER_OUTPUT_WIDTH   1280
#define MUXER_OUTPUT_HEIGHT  720
#define MUXER_BATCH_TIMEOUT  33000   /* µs */
#define PGIE_CONFIG_FILE     "/app/configs/config_infer_primary_yoloV8_face.txt"
#define OUTPUT_DIR           "/app/outputs/crops"
#define GPU_ID               0       /* GPU ID for nvds_obj_encode */
#define MAX_SOURCES          16      /* Maximum number of video sources */
#define SAVE_INTERVAL_SEC    5       /* Seconds between frame saves per source */
/* ──────────────────────────────────────────────────────────────── */

/* Global encode context (one per process is enough for this demo) */
static NvDsObjEncCtxHandle g_enc_ctx = NULL;
static guint               g_frame_count = 0;

/* Per-source save control - similar to ImageMetaConsumer */
static struct timespec     g_last_save_time[MAX_SOURCES] = {0};
static pthread_mutex_t     g_source_mutex[MAX_SOURCES];

/* ── helper: save every detected object crop ───────────────────── */
/**
 * Will save an image (full frame or cropped object) using nvds_obj_encode.
 * If the path is too long, the save will not occur and an error message will be displayed.
 * 
 * @param path Where the image will be saved (must be null-terminated C string)
 * @param ip_surf Object containing the image to save
 * @param obj_meta Object containing information about the area to crop (NULL for full frame)
 * @param frame_meta Object containing information about the current frame
 * @param obj_counter Pointer to counter for tracking saved objects (incremented on success)
 * @return TRUE if the image was queued for encoding, FALSE otherwise
 */
static gboolean
save_image (const gchar *path,
            NvBufSurface *ip_surf,
            NvDsObjectMeta *obj_meta,
            NvDsFrameMeta *frame_meta,
            guint *obj_counter)
{
    NvDsObjEncUsrArgs userData = {0};
    size_t path_len = strlen (path);
    
    if (path_len >= sizeof (userData.fileNameImg)) {
        g_printerr ("[ERROR] Path too long (%s, size: %zu). Must be < %zu characters.\n",
                    path, path_len, sizeof (userData.fileNameImg));
        return FALSE;
    }
    
    /* If obj_meta is NULL, save the full frame */
    if (obj_meta == NULL) {
        userData.isFrame = 1;
    }
    
    userData.saveImg = TRUE;
    userData.attachUsrMeta = FALSE;
    strncpy (userData.fileNameImg, path, sizeof (userData.fileNameImg) - 1);
    userData.fileNameImg[sizeof (userData.fileNameImg) - 1] = '\0';
    userData.objNum = (*obj_counter)++;
    userData.quality = 80;


    g_print("[INFO] Queueing image for encoding: %s (objNum: %u, batch_id: %u)\n", 
             path, userData.objNum, frame_meta->batch_id);
    fflush(stdout);
    bool ret = nvds_obj_enc_process (g_enc_ctx, &userData, ip_surf, obj_meta, frame_meta);

    if (!ret) {
        g_printerr ("[ERROR] nvds_obj_enc_process failed for %s\n", path);
        return FALSE;
    }
    g_print("[INFO] Image queued successfully: %s\n", path);
    fflush(stdout);
    
    return TRUE;
}

/* ── Per-source save control (ref: image_meta_consumer.cpp) ──────── */

/**
 * Initialize per-source save control structures.
 * Should be called before pipeline starts.
 */
static void
init_source_save_control(void)
{
    for (unsigned i = 0; i < MAX_SOURCES; i++) {
        pthread_mutex_init(&g_source_mutex[i], NULL);
        /* Initialize to epoch (will cause first frame to be saved) */
        g_last_save_time[i].tv_sec = 0;
        g_last_save_time[i].tv_nsec = 0;
    }
}

/**
 * Cleanup per-source save control structures.
 * Should be called during shutdown.
 */
static void
cleanup_source_save_control(void)
{
    for (unsigned i = 0; i < MAX_SOURCES; i++) {
        pthread_mutex_destroy(&g_source_mutex[i]);
    }
}

/**
 * Check if enough time has passed since last save for this source.
 * Based on ImageMetaConsumer::should_save_data logic.
 */
static gboolean
should_save_data(unsigned source_number)
{
    if (source_number >= MAX_SOURCES) {
        g_printerr("[WARNING] Invalid source_number %u (max: %u)\n", 
                   source_number, MAX_SOURCES - 1);
        return FALSE;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    /* Check if current time moved backwards */
    if (now.tv_sec < g_last_save_time[source_number].tv_sec) {
        g_printerr("[WARNING] Time moved backwards for source %u. Resetting.\n", 
                   source_number);
        g_last_save_time[source_number] = now;
        return TRUE;
    }

    /* Calculate time elapsed since last save */
    time_t elapsed_sec = now.tv_sec - g_last_save_time[source_number].tv_sec;

    /* Save if interval has passed */
    return (elapsed_sec >= SAVE_INTERVAL_SEC);
}

/**
 * Lock mutex for a specific source.
 * Ref: ImageMetaConsumer::lock_source_nb
 */
static void
lock_source_nb(unsigned source_number)
{
    if (source_number >= MAX_SOURCES) {
        g_printerr("[WARNING] Invalid source_number %u for lock\n", source_number);
        return;
    }
    pthread_mutex_lock(&g_source_mutex[source_number]);
}

/**
 * Unlock mutex for a specific source.
 * Ref: ImageMetaConsumer::unlock_source_nb
 */
static void
unlock_source_nb(unsigned source_number)
{
    if (source_number >= MAX_SOURCES) {
        g_printerr("[WARNING] Invalid source_number %u for unlock\n", source_number);
        return;
    }
    pthread_mutex_unlock(&g_source_mutex[source_number]);
}

/**
 * Mark that data was saved for this source (update timestamp).
 * Ref: ImageMetaConsumer::data_was_saved_for_source
 */
static void
data_was_saved_for_source(unsigned source_number)
{
    if (source_number >= MAX_SOURCES) {
        return;
    }
    clock_gettime(CLOCK_REALTIME, &g_last_save_time[source_number]);
}

static void
save_object_crops (GstBuffer *buf, NvDsBatchMeta *batch_meta)
{
    NvDsObjectMeta *obj_meta = NULL;
    gboolean at_least_one_image_saved = FALSE;

    /* Extract NvBufSurface from GstBuffer */
    GstMapInfo inmap = GST_MAP_INFO_INIT;
    if (!gst_buffer_map(buf, &inmap, GST_MAP_READ)) {
        g_printerr ("[ERROR] Failed to map input buffer\n");
        return;
    }
    NvBufSurface *ip_surf = (NvBufSurface *) inmap.data;

    // Iterate through frames in batch
    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL;
         l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);
        unsigned source_number = frame_meta->pad_index;

        if (should_save_data(source_number)) {
            lock_source_nb(source_number);
            if (!should_save_data(source_number)) {
                unlock_source_nb(source_number);
                continue;
            }
        } else {
            continue;
        }

        gboolean at_least_one_object_detected = FALSE;
        gboolean full_frame_written = FALSE;
        guint obj_counter = 0;

        /* First pass: check if there are any detected objects */
        for (NvDsMetaList *ol = frame_meta->obj_meta_list; ol; ol = ol->next) {
            obj_meta = (NvDsObjectMeta *) ol->data;
            at_least_one_object_detected = TRUE;
            break;
        }

        /* Only process if objects were detected */
        if (at_least_one_object_detected) {
            /* Build output path for full frame */
            gchar frame_path[512];
            snprintf (frame_path, sizeof (frame_path),
                      "%s/frame%04u.jpg",
                      OUTPUT_DIR,
                      g_frame_count);

            /* Second pass: save cropped images and full frame */
            for (NvDsMetaList *ol = frame_meta->obj_meta_list; ol; ol = ol->next) {
                obj_meta = (NvDsObjectMeta *) ol->data;

                /* Save full frame once per frame (on first object) */
                if (!full_frame_written) {
                    g_print ("[INFO] Saving full frame %u to %s\n",
                             frame_meta->frame_num, frame_path);

                    if (save_image (frame_path, ip_surf, NULL, frame_meta, &obj_counter)) {
                        at_least_one_image_saved = TRUE;
                    }
                    full_frame_written = TRUE;
                }
            }
        }

        /* Mark that we saved data for this source */
        if (at_least_one_object_detected) {
            data_was_saved_for_source(source_number);
        }

        g_frame_count++;
        unlock_source_nb(source_number);
    }

    /* Wait for all encoding threads to finish writing JPEG files */
    if (at_least_one_image_saved) {
        g_print("[INFO] Waiting for all images to be saved for batch. Total frames processed: %u\n", g_frame_count);
        nvds_obj_enc_finish (g_enc_ctx);
        g_print("[INFO] All images saved for batch. Total frames processed: %u\n", g_frame_count);
    }

    gst_buffer_unmap(buf, &inmap);
}

/* ── appsink "new-sample" callback ─────────────────────────────── */
static GstFlowReturn
on_new_sample (GstElement *appsink, gpointer user_data)
{
    GstSample *sample = NULL;
    g_signal_emit_by_name (appsink, "pull-sample", &sample);
    if (!sample)
        return GST_FLOW_ERROR;

    GstBuffer *buf = gst_sample_get_buffer (sample);
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);

    if (batch_meta)
        save_object_crops (buf, batch_meta);

    gst_sample_unref (sample);
    return GST_FLOW_OK;
}

/* ── bus watch ─────────────────────────────────────────────────── */
static gboolean
bus_call (GstBus *bus, GstMessage *msg, gpointer loop)
{
    switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
        g_print ("[INFO] End-of-stream\n");
        g_main_loop_quit ((GMainLoop *) loop);
        break;
    case GST_MESSAGE_ERROR: {
        gchar *debug = NULL;
        GError *err  = NULL;
        gst_message_parse_error (msg, &err, &debug);
        g_printerr ("[ERROR] %s\n", err->message);
        g_printerr ("[DEBUG] %s\n", debug ? debug : "none");
        g_free (debug);
        g_error_free (err);
        g_main_loop_quit ((GMainLoop *) loop);
        break;
    }
    default:
        break;
    }
    return TRUE;
}

/* ── qtdemux pad-added callback ───────────────────────────────── */
static void
on_pad_added (GstElement *element, GstPad *pad, gpointer data)
{
    GstElement *h264parser = (GstElement *) data;
    GstPad *sink_pad = gst_element_get_static_pad (h264parser, "sink");
    
    if (!gst_pad_is_linked (sink_pad)) {
        GstCaps *caps = gst_pad_get_current_caps (pad);
        if (caps) {
            GstStructure *str = gst_caps_get_structure (caps, 0);
            const gchar *name = gst_structure_get_name (str);
            
            /* Link only video pads */
            if (g_str_has_prefix (name, "video/")) {
                if (gst_pad_link (pad, sink_pad) == GST_PAD_LINK_OK) {
                    g_print ("[INFO] Linked qtdemux video pad\n");
                } else {
                    g_printerr ("[ERROR] Failed to link qtdemux → h264parser\n");
                }
            }
            gst_caps_unref (caps);
        }
    }
    
    gst_object_unref (sink_pad);
}

/* ── main ──────────────────────────────────────────────────────── */
int
main (int argc, char *argv[])
{
    if (argc < 2) {
        g_printerr ("Usage: %s <input_mp4_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Create output directory */
    g_mkdir_with_parents (OUTPUT_DIR, 0755);

    /* Initialize per-source save control (ref: image_meta_consumer) */
    init_source_save_control();

    gst_init (&argc, &argv);
    GMainLoop *loop = g_main_loop_new (NULL, FALSE);

    /* ── Create elements ─────────────────────────────────────── */
    GstElement *pipeline      = gst_pipeline_new ("deepstream-pipeline");
    GstElement *source        = gst_element_factory_make ("filesrc",        "file-source");
    GstElement *qtdemux       = gst_element_factory_make ("qtdemux",        "qt-demuxer");
    GstElement *h264parser    = gst_element_factory_make ("h264parse",      "h264-parser");
    GstElement *decoder       = gst_element_factory_make ("nvv4l2decoder",  "nvv4l2-decoder");
    GstElement *streammux     = gst_element_factory_make ("nvstreammux",    "stream-muxer");
    GstElement *pgie          = gst_element_factory_make ("nvinfer",        "primary-nvinference-engine");
    GstElement *nvvidconv     = gst_element_factory_make ("nvvideoconvert", "nvvideo-converter");
    GstElement *nvosd         = gst_element_factory_make ("nvdsosd",        "nv-onscreendisplay");
    GstElement *capsfilter    = gst_element_factory_make ("capsfilter",     "caps-filter");
    GstElement *sink          = gst_element_factory_make ("appsink",        "app-sink");

    if (!pipeline || !source || !qtdemux || !h264parser || !decoder ||
        !streammux || !pgie  || !nvvidconv  || !nvosd   || !capsfilter || !sink) {
        g_printerr ("[ERROR] Failed to create one or more elements.\n");
        return EXIT_FAILURE;
    }

    /* ── Configure elements ──────────────────────────────────── */
    g_object_set (source,    "location",         argv[1],               NULL);
    g_object_set (streammux, "width",            MUXER_OUTPUT_WIDTH,
                             "height",           MUXER_OUTPUT_HEIGHT,
                             "batch-size",       1,
                             "batched-push-timeout", MUXER_BATCH_TIMEOUT, NULL);
    g_object_set (pgie,      "config-file-path", PGIE_CONFIG_FILE,      NULL);

    /* capsfilter: force NVMM memory to ensure appsink gets correct buffer type */
    GstCaps *caps = gst_caps_from_string ("video/x-raw(memory:NVMM), format=RGB");
    g_object_set (capsfilter, "caps", caps, NULL);
    gst_caps_unref (caps);

    /* appsink: emit signal, keep last sample only */
    g_object_set (sink, "emit-signals", TRUE, "sync", FALSE, NULL);
    g_signal_connect (sink, "new-sample", G_CALLBACK (on_new_sample), NULL);

    /* ── Bus ─────────────────────────────────────────────────── */
    GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    gst_bus_add_watch (bus, bus_call, loop);
    gst_object_unref (bus);

    /* ── Add elements to pipeline ────────────────────────────── */
    gst_bin_add_many (GST_BIN (pipeline),
                      source, qtdemux, h264parser, decoder, streammux,
                      pgie, nvvidconv, nvosd, capsfilter, sink, NULL);

    /* ── Link: filesrc → qtdemux ────────────────────────────── */
    if (!gst_element_link (source, qtdemux)) {
        g_printerr ("[ERROR] Failed to link source → qtdemux\n");
        return EXIT_FAILURE;
    }

    /* qtdemux → h264parser (dynamic pad) */
    g_signal_connect (qtdemux, "pad-added", G_CALLBACK (on_pad_added), h264parser);

    /* h264parser → nvv4l2decoder */
    if (!gst_element_link (h264parser, decoder)) {
        g_printerr ("[ERROR] Failed to link h264parser → decoder\n");
        return EXIT_FAILURE;
    }

    /* decoder → muxer (request pad) */
    GstPad *sinkpad  = gst_element_request_pad_simple (streammux, "sink_0");
    GstPad *srcpad   = gst_element_get_static_pad  (decoder,   "src");
    if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK) {
        g_printerr ("[ERROR] Failed to link decoder → streammux\n");
        return EXIT_FAILURE;
    }
    gst_object_unref (sinkpad);
    gst_object_unref (srcpad);

    /* muxer → nvinfer → nvvideoconvert → nvdsosd → capsfilter → appsink */
    if (!gst_element_link_many (streammux, pgie, nvvidconv,    nvosd, 

        sink, NULL)) {
        g_printerr ("[ERROR] Failed to link muxer → pgie → … → sink\n");
        return EXIT_FAILURE;
    }

    /* ── Init nvds_obj_encode context ───────────────────────── */
    g_enc_ctx = nvds_obj_enc_create_context (GPU_ID);
    if (!g_enc_ctx) {
        g_printerr ("[ERROR] Failed to create nvds_obj_encode context\n");
        return EXIT_FAILURE;
    }

    /* ── Run ─────────────────────────────────────────────────── */
    g_print ("[INFO] Starting pipeline…  input: %s\n", argv[1]);
    gst_element_set_state (pipeline, GST_STATE_PLAYING);
    g_main_loop_run (loop);

    /* ── Cleanup ─────────────────────────────────────────────── */
    g_print ("[INFO] Shutting down. Frames processed: %u\n", g_frame_count);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    nvds_obj_enc_destroy_context (g_enc_ctx);
    cleanup_source_save_control();
    gst_object_unref (pipeline);
    g_main_loop_unref (loop);

    return EXIT_SUCCESS;
}