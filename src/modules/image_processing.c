#include <stdio.h>
#include "image_processing.h"
#include <jpeglib.h>
#include <setjmp.h>
#include "nvbufsurftransform.h"
#include <cuda_runtime_api.h>

// Reuse the debug category defined in deepstream.cpp
GST_DEBUG_CATEGORY_EXTERN(deepstream_debug_category);
#define GST_CAT_DEFAULT deepstream_debug_category

// ── Helper function declarations ─────────────────────────────────────────────
static gboolean validate_surface(NvBufSurface *surface, guint batch_id);
static gboolean build_frame_output_paths(NvDsFrameMeta *frame_meta, const gchar *base_output_dir, gchar **rel_path, gchar **abs_path);
static NvBufSurface *create_rgba_surface(guint gpu_id, guint width, guint height);
static NvBufSurfTransform_Error transform_batch_slot(NvBufSurface *surface, NvBufSurface *dst_surface, guint batch_id, NvBufSurfTransformParams *tp);
static guchar *copy_surface_to_cpu(NvBufSurfaceParams *params);
static guchar *rgba_to_rgb(const guchar *rgba_buf, guint width, guint height, guint pitch);
static void encode_rgb_to_jpeg_mem(const guchar *rgb_data, guint width, guint height, gint quality, unsigned char **out_buf, unsigned long *out_size);
static gboolean encode_rgb_to_jpeg_file(const guchar *rgb_data, guint width, guint height, gint quality, const gchar *path);
static gboolean validate_and_clamp_crop_box(CropBox *crop_box, const NvBufSurfaceParams *surf_params);

/* ── Public API ──────────────────────────────────────────────────────────── */
gchar *
encode_crop_to_base64_jpeg(NvBufSurface *surface, CropBox *crop_box, gint quality, guint batch_id)
{
  if (!validate_surface(surface, batch_id))
    return NULL;

  NvBufSurfaceParams *surf_params = &surface->surfaceList[batch_id];
  if (!validate_and_clamp_crop_box(crop_box, surf_params))
    return NULL;

  NvBufSurface *dst_surface = create_rgba_surface(surface->gpuId,
                                                   crop_box->width,
                                                   crop_box->height);
  if (!dst_surface)
    return NULL;


  
  // Transform the specified crop region from the input surface to a new surface
  NvBufSurfTransformRect src_rect = { crop_box->top,  crop_box->left,
                                      crop_box->width, crop_box->height };
  NvBufSurfTransformRect dst_rect = { 0, 0, crop_box->width, crop_box->height };

  NvBufSurfTransformParams tp = {0};
  tp.src_rect        = &src_rect;
  tp.dst_rect        = &dst_rect;
  tp.transform_flag  = NVBUFSURF_TRANSFORM_CROP_SRC |
                       NVBUFSURF_TRANSFORM_CROP_DST |
                       NVBUFSURF_TRANSFORM_FILTER;
  tp.transform_filter = NvBufSurfTransformInter_Default;

  NvBufSurfTransform_Error err = transform_batch_slot(surface, dst_surface, batch_id, &tp);
  if (err != NvBufSurfTransformError_Success) {
    GST_ERROR("Surface transform failed for batch_id=%u, error=%d", batch_id, err);
    NvBufSurfaceDestroy(dst_surface);
    return NULL;
  }

  cudaStreamSynchronize(0);
  
  // Copy transformed surface data to CPU and encode to JPEG
  NvBufSurfaceParams *dst_params = &dst_surface->surfaceList[0];
  guint width = dst_params->width;
  guint height = dst_params->height;
  guint pitch = dst_params->pitch;
  guchar *cpu_buf = copy_surface_to_cpu(dst_params);
  NvBufSurfaceDestroy(dst_surface);
  if (!cpu_buf)
    return NULL;

  guchar *rgb_data = rgba_to_rgb(cpu_buf, width, height, pitch);
  g_free(cpu_buf);
  if (!rgb_data)
    return NULL;

  // Encode RGB data to JPEG in memory
  unsigned char *jpeg_buf  = NULL;
  unsigned long  jpeg_size = 0;
  encode_rgb_to_jpeg_mem(rgb_data, width, height,
                         quality, &jpeg_buf, &jpeg_size);
  g_free(rgb_data);

  gchar *base64 = g_base64_encode(jpeg_buf, jpeg_size);
  free(jpeg_buf);

  return base64;
}

gchar *
save_frame_to_jpeg(NvBufSurface *surface, NvDsFrameMeta *frame_meta,
                   const gchar *base_output_dir, gint quality,
                  gboolean exclude_letterbox, LetterboxGeometry *lb_geom )
{
  if (!surface || !frame_meta || !base_output_dir) {
    GST_ERROR("Invalid parameters for save_frame_to_jpeg");
    return NULL;
  }

  guint batch_id = frame_meta->batch_id;
  if (!validate_surface(surface, batch_id))
    return NULL;


  gchar *relative_path = NULL;
  gchar *absolute_path = NULL;
  if (!build_frame_output_paths(frame_meta, base_output_dir,
                                &relative_path, &absolute_path))
    return NULL;

  // Create RGBA surface
  NvBufSurfaceParams *src_params = &surface->surfaceList[batch_id];
  
  guint width = exclude_letterbox ? lb_geom->content_w : src_params->width;
  guint height = exclude_letterbox ? lb_geom->content_h : src_params->height;

  NvBufSurface *dst_surface = create_rgba_surface(surface->gpuId, width, height);
  if (!dst_surface) {
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }


  // Set up transform parameters to copy the entire source surface to the destination RGBA surface
  NvBufSurfTransformRect src_rect = { 
    exclude_letterbox ? lb_geom->pad_y : 0,
    exclude_letterbox ? lb_geom->pad_x : 0, 
    exclude_letterbox ? lb_geom->content_w : src_params->width, 
    exclude_letterbox ? lb_geom->content_h : src_params->height };
  NvBufSurfTransformRect dst_rect = { 0, 0, width, height }; 

  NvBufSurfTransformParams transform_params = {0};
  transform_params.src_rect = &src_rect;
  transform_params.dst_rect = &dst_rect;
  transform_params.transform_flag = NVBUFSURF_TRANSFORM_CROP_SRC |
                    NVBUFSURF_TRANSFORM_CROP_DST |
                    NVBUFSURF_TRANSFORM_FILTER;
  transform_params.transform_filter = NvBufSurfTransformInter_Default;

  NvBufSurfTransform_Error err = transform_batch_slot(surface, dst_surface, batch_id, &transform_params);
  if (err != NvBufSurfTransformError_Success) {
    GST_ERROR("Surface transform failed for batch_id=%u, error=%d", batch_id, err);
    NvBufSurfaceDestroy(dst_surface);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  cudaStreamSynchronize(0); // Ensure transform is complete before accessing data

  //=================================================
  // Copy transformed surface data to CPU and encode to JPEG
  NvBufSurfaceParams *dst_params = &dst_surface->surfaceList[0];
  guint pitch = dst_params->pitch;
  GST_INFO("Transformed surface: width=%u, height=%u, pitch=%u\n", width, height, pitch);

  // =================================================
  guchar *cpu_buffer = copy_surface_to_cpu(dst_params);
  NvBufSurfaceDestroy(dst_surface);
  if (!cpu_buffer) {
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  //=================================================
  // Convert RGBA to RGB by stripping alpha channel
  guchar *rgb_data = rgba_to_rgb(cpu_buffer, width, height, pitch);
  g_free(cpu_buffer);
  if (!rgb_data) {
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  //=================================================
  // Encode RGB data to JPEG and save to file
  gboolean ok = encode_rgb_to_jpeg_file(rgb_data, width, height,
                                        quality, absolute_path);
  g_free(rgb_data);
  g_free(absolute_path);

  if (!ok) {
    g_free(relative_path);
    return NULL;
  }
  return relative_path;
}


//==================================================
// ── Helper function implementations ──────────────────────────────────────────
//==================================================

static gboolean
validate_and_clamp_crop_box(CropBox *crop_box, const NvBufSurfaceParams *surf_params)
{
  if (crop_box->left >= surf_params->width || 
      crop_box->top >= surf_params->height ||
      crop_box->width == 0 || crop_box->height == 0) {
    GST_ERROR("Invalid crop box: left=%u top=%u width=%u height=%u (surface: %dx%d)",
              crop_box->left, crop_box->top, 
              crop_box->width, crop_box->height,
              surf_params->width, surf_params->height);
    return FALSE;
  }

  if (crop_box->left + crop_box->width > surf_params->width)
    crop_box->width = surf_params->width - crop_box->left;
  if (crop_box->top + crop_box->height > surf_params->height)
    crop_box->height = surf_params->height - crop_box->top;

  if (crop_box->width < 10 || crop_box->height < 10) {
    GST_ERROR("Crop box too small: %ux%u", crop_box->width, crop_box->height);
    return FALSE;
  }

  return TRUE;
}

// function to create a directory if it doesn't exist, and return TRUE if the directory is ready for use
gboolean
ensure_frame_save_directory(const gchar *dir_path)
{
  if (!dir_path) {
    return FALSE;
  }
  
  if (g_file_test(dir_path, G_FILE_TEST_IS_DIR)) {
    return TRUE;
  }
  
  if (g_mkdir_with_parents(dir_path, 0755) != 0) {
    GST_ERROR("Failed to create directory: %s", dir_path);
    return FALSE;
  }
  
  GST_INFO("Created frame save directory: %s", dir_path);
  return TRUE;
}


/* ── Surface validation ──────────────────────────────────────────────────── */

static gboolean
validate_surface(NvBufSurface *surface, guint batch_id)
{
  if (!surface) {
    GST_ERROR("Surface is NULL");
    return FALSE;
  }

  if (surface->numFilled < 1) {
    GST_ERROR("Surface has no filled buffers (numFilled=%d)", surface->numFilled);
    return FALSE;
  }

  if (!surface->surfaceList) {
    GST_ERROR("Surface list is NULL");
    return FALSE;
  }

  if (batch_id >= surface->numFilled) {
    GST_ERROR("Invalid batch_id %u (numFilled=%d)", batch_id, surface->numFilled);
    return FALSE;
  }

  NvBufSurfaceParams *p = &surface->surfaceList[batch_id];
  if (p->width == 0 || p->height == 0) {
    GST_ERROR("Surface has invalid dimensions: %dx%d", p->width, p->height);
    return FALSE;
  }

  return TRUE;
}

/* ── Path helpers ────────────────────────────────────────────────────────── */

/**
 * Build per-source output directory and frame file paths.
 * On success returns TRUE and sets *rel_path and *abs_path (both caller-owned).
 */
static gboolean
build_frame_output_paths(NvDsFrameMeta *frame_meta, const gchar *base_output_dir,
                         gchar **rel_path, gchar **abs_path)
{
  guint    source_id = frame_meta->source_id;
  guint    frame_num = frame_meta->frame_num;
  gboolean infer_ok  = frame_meta->bInferDone;

  gchar *rel_dir = g_strdup_printf("source_%u", source_id);
  gchar *abs_dir = g_strdup_printf("%s/%s", base_output_dir, rel_dir);

  if (!ensure_frame_save_directory(abs_dir)) {
    GST_ERROR("Failed to create directory: %s", abs_dir);
    g_free(rel_dir);
    g_free(abs_dir);
    return FALSE;
  }

  GDateTime *now       = g_date_time_new_now_local();
  gchar     *timestamp = g_date_time_format(now, "%Y%m%d_%H%M%S");
  g_date_time_unref(now);

  gchar *filename = g_strdup_printf("frame_src%u_num%u_%d_%s.jpg",
                                    source_id, frame_num, infer_ok, timestamp);
  g_free(timestamp);

  *rel_path = g_strdup_printf("%s/%s", rel_dir, filename);
  *abs_path = g_strdup_printf("%s/%s", abs_dir, filename);

  g_free(rel_dir);
  g_free(abs_dir);
  g_free(filename);

  return TRUE;
}


/* ── Surface creation & transform ────────────────────────────────────────── */
static NvBufSurface *
create_rgba_surface(guint gpu_id, guint width, guint height)
{
  NvBufSurface *dst = NULL;
  NvBufSurfaceCreateParams p = {0};
  p.gpuId        = gpu_id;
  p.width        = width;
  p.height       = height;
  p.isContiguous = 1;
  p.colorFormat  = NVBUF_COLOR_FORMAT_RGBA;
  p.layout       = NVBUF_LAYOUT_PITCH;
#ifdef __aarch64__
  p.memType = NVBUF_MEM_DEFAULT;
#else
  p.memType = NVBUF_MEM_CUDA_DEVICE;
#endif

  if (NvBufSurfaceCreate(&dst, 1, &p) != 0) {
    GST_ERROR("Failed to create destination RGBA surface (%ux%u)", width, height);
    return NULL;
  }
  return dst;
}

/**
 * Run NvBufSurfTransform for a specific batch slot.
 * The API requires batchSize==1, so we temporarily swap slot 0, call the
 * transform, then restore the original batch state.
 */
static NvBufSurfTransform_Error
transform_batch_slot(NvBufSurface *surface, NvBufSurface *dst_surface,
                     guint batch_id, NvBufSurfTransformParams *tp)
{
  NvBufSurfTransformConfigParams cfg = {0};
  cfg.compute_mode = NvBufSurfTransformCompute_Default;
  cfg.gpu_id       = surface->gpuId;
  cfg.cuda_stream  = NULL; // Use default stream for simplicity, but consider using a dedicated stream for better performance in a real application

  if (NvBufSurfTransformSetSessionParams(&cfg) != 0) {
    GST_ERROR("Failed to set transform session params");
    return NvBufSurfTransformError_Invalid_Params;
  }

  /* Temporarily expose only the requested batch slot */
  NvBufSurfaceParams orig_first = surface->surfaceList[0];
  guint orig_filled = surface->numFilled;
  guint orig_batch  = surface->batchSize;

  if (batch_id > 0)
    surface->surfaceList[0] = surface->surfaceList[batch_id];
  surface->numFilled = 1;
  surface->batchSize = 1;

  NvBufSurfTransform_Error err = NvBufSurfTransform(surface, dst_surface, tp);

  surface->surfaceList[0] = orig_first;
  surface->numFilled      = orig_filled;
  surface->batchSize      = orig_batch;

  return err;
}

/* ── CPU pixel helpers ───────────────────────────────────────────────────── */

/**
 * Copy surface GPU memory to a newly allocated CPU buffer.
 * Caller must g_free() the returned pointer.
 */
static guchar *
copy_surface_to_cpu(NvBufSurfaceParams *params)
{
  guint size = params->pitch * params->height;
  guchar *buf = g_malloc(size);
  if (!buf) {
    GST_ERROR("Failed to allocate CPU buffer (%u bytes)", size);
    return NULL;
  }

  cudaError_t e = cudaMemcpy(buf, params->dataPtr, size, cudaMemcpyDeviceToHost);
  if (e != cudaSuccess) {
    GST_ERROR("cudaMemcpy failed: %s", cudaGetErrorString(e));
    g_free(buf);
    return NULL;
  }

  return buf;
}

/**
 * Convert packed RGBA (pitch-aligned) to tightly-packed RGB.
 * Caller must g_free() the returned pointer.
 */
static guchar *
rgba_to_rgb(const guchar *rgba_buf, guint width, guint height, guint pitch)
{
  guint rgb_stride = width * 3;
  guchar *rgb = (guchar *)g_malloc(rgb_stride * height);
  if (!rgb) {
    GST_ERROR("Failed to allocate RGB buffer");
    return NULL;
  }

  for (guint y = 0; y < height; y++) {
    const guchar *src = rgba_buf + y * pitch;
    guchar       *dst = rgb      + y * rgb_stride;
    for (guint x = 0; x < width; x++) {
      dst[x * 3 + 0] = src[x * 4 + 0];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }

  return rgb;
}


/* ── JPEG encoding ───────────────────────────────────────────────────────── */

/**
 * Encode RGB pixels to a JPEG memory buffer.
 * On success, *out_buf is libjpeg-allocated (caller must free() it) and
 * *out_size contains the number of bytes.
 */
static void
encode_rgb_to_jpeg_mem(const guchar *rgb_data, guint width, guint height,
                       gint quality,
                       unsigned char **out_buf, unsigned long *out_size)
{
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr       jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, out_buf, out_size);

  cinfo.image_width      = width;
  cinfo.image_height     = height;
  cinfo.input_components = 3;
  cinfo.in_color_space   = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  guint rgb_stride = width * 3;
  JSAMPROW row[1];
  while (cinfo.next_scanline < cinfo.image_height) {
    row[0] = (JSAMPROW)(rgb_data + cinfo.next_scanline * rgb_stride);
    jpeg_write_scanlines(&cinfo, row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
}


/**
 * Encode RGB pixels to a JPEG file.
 */
static gboolean
encode_rgb_to_jpeg_file(const guchar *rgb_data, guint width, guint height,
                        gint quality, const gchar *path)
{
  FILE *outfile = fopen(path, "wb");
  if (!outfile) {
    GST_ERROR("Failed to open output file: %s", path);
    return FALSE;
  }

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr       jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, outfile);

  cinfo.image_width      = width;
  cinfo.image_height     = height;
  cinfo.input_components = 3;
  cinfo.in_color_space   = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  guint rgb_stride = width * 3;
  JSAMPROW row[1];
  while (cinfo.next_scanline < cinfo.image_height) {
    row[0] = (JSAMPROW)(rgb_data + cinfo.next_scanline * rgb_stride);
    jpeg_write_scanlines(&cinfo, row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  fclose(outfile);

  return TRUE;
}
