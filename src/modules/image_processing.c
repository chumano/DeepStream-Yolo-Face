#include <stdio.h>
#include "image_processing.h"
#include <jpeglib.h>
#include <setjmp.h>
#include "nvbufsurftransform.h"
#include <cuda_runtime_api.h>

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

void
calculate_crop_box(NvDsObjectMeta *obj_meta, CropBox *crop_box, guint frame_width, guint frame_height)
{
  // Add 20% padding around the bounding box
  gfloat padding = 0.2f;
  
  gfloat pad_w = obj_meta->rect_params.width * padding;
  gfloat pad_h = obj_meta->rect_params.height * padding;
  
  gint left = (gint)(obj_meta->rect_params.left - pad_w);
  gint top = (gint)(obj_meta->rect_params.top - pad_h);
  gint right = (gint)(obj_meta->rect_params.left + obj_meta->rect_params.width + pad_w);
  gint bottom = (gint)(obj_meta->rect_params.top + obj_meta->rect_params.height + pad_h);
  
  // Clamp to frame boundaries
  crop_box->left = MAX(0, left);
  crop_box->top = MAX(0, top);
  crop_box->width = MIN(right, (gint)frame_width) - crop_box->left;
  crop_box->height = MIN(bottom, (gint)frame_height) - crop_box->top;
}

gchar *
encode_crop_to_base64_jpeg(NvBufSurface *surface, CropBox *crop_box, gint quality, guint batch_id)
{
  if (!surface) {
    GST_ERROR("Surface is NULL");
    return NULL;
  }

  if (surface->numFilled < 1) {
    GST_ERROR("Surface has no filled buffers (numFilled=%d)", surface->numFilled);
    return NULL;
  }

  if (!surface->surfaceList) {
    GST_ERROR("Surface list is NULL");
    return NULL;
  }

  if (batch_id >= surface->numFilled) {
    GST_ERROR("Invalid batch_id %u (numFilled=%d)", batch_id, surface->numFilled);
    return NULL;
  }

  NvBufSurfaceParams *surf_params = &surface->surfaceList[batch_id];

  if (!surf_params) {
    GST_ERROR("Surface params is NULL");
    return NULL;
  }

  if (surf_params->width == 0 || surf_params->height == 0) {
    GST_ERROR("Surface has invalid dimensions: %dx%d", surf_params->width, surf_params->height);
    return NULL;
  }

  // ...existing validation code...

  if (crop_box->left >= surf_params->width || crop_box->top >= surf_params->height ||
      crop_box->width == 0 || crop_box->height == 0) {
    GST_ERROR("Invalid crop box: left=%u, top=%u, width=%u, height=%u (surface: %dx%d)",
               crop_box->left, crop_box->top, crop_box->width, crop_box->height,
               surf_params->width, surf_params->height);
    return NULL;
  }

  if (crop_box->left + crop_box->width > surf_params->width) {
    crop_box->width = surf_params->width - crop_box->left;
  }
  if (crop_box->top + crop_box->height > surf_params->height) {
    crop_box->height = surf_params->height - crop_box->top;
  }

  if (crop_box->width < 10 || crop_box->height < 10) {
    GST_ERROR("Crop box too small: %ux%u", crop_box->width, crop_box->height);
    return NULL;
  }

  // ...existing surface creation and transform code...
  NvBufSurface *dst_surface = NULL;
  NvBufSurfaceCreateParams create_params = {0};
  create_params.gpuId = surface->gpuId;
  create_params.width = crop_box->width;
  create_params.height = crop_box->height;
  create_params.size = 0;
  create_params.isContiguous = 1;
  create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create_params.layout = NVBUF_LAYOUT_PITCH;
#ifdef __aarch64__
  create_params.memType = NVBUF_MEM_DEFAULT;
#else
  create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
#endif
  
  if (NvBufSurfaceCreate(&dst_surface, 1, &create_params) != 0) {
    GST_ERROR("Failed to create destination surface");
    return NULL;
  }
  
  // ...existing transform code...
  NvBufSurfTransformParams transform_params = {0};
  NvBufSurfTransformRect src_rect = {0};
  NvBufSurfTransformRect dst_rect = {0};
  
  src_rect.top = crop_box->top;
  src_rect.left = crop_box->left;
  src_rect.width = crop_box->width;
  src_rect.height = crop_box->height;
  
  dst_rect.top = 0;
  dst_rect.left = 0;
  dst_rect.width = crop_box->width;
  dst_rect.height = crop_box->height;
  
  transform_params.src_rect = &src_rect;
  transform_params.dst_rect = &dst_rect;
  transform_params.transform_flag = NVBUFSURF_TRANSFORM_CROP_SRC |
                                    NVBUFSURF_TRANSFORM_CROP_DST |
                                    NVBUFSURF_TRANSFORM_FILTER;
  transform_params.transform_filter = NvBufSurfTransformInter_Default;
  
  NvBufSurfTransformConfigParams config_params = {0};
  config_params.compute_mode = NvBufSurfTransformCompute_Default;
  config_params.gpu_id = surface->gpuId;
  config_params.cuda_stream = NULL;
  
  if (NvBufSurfTransformSetSessionParams(&config_params) != 0) {
    GST_ERROR("Failed to set transform session params");
    NvBufSurfaceDestroy(dst_surface);
    return NULL;
  }
  
  NvBufSurfaceParams orig_first = surface->surfaceList[0];
  guint orig_numFilled = surface->numFilled;
  guint orig_batchSize = surface->batchSize;

  if (batch_id > 0) {
    surface->surfaceList[0] = surface->surfaceList[batch_id];
  }
  surface->numFilled = 1;
  surface->batchSize = 1;

  NvBufSurfTransform_Error transform_err = NvBufSurfTransform(surface, dst_surface, &transform_params);
  
  surface->surfaceList[0] = orig_first;
  surface->numFilled = orig_numFilled;
  surface->batchSize = orig_batchSize;

  if (transform_err != NvBufSurfTransformError_Success) {
    GST_ERROR("Failed to transform surface for batch_id=%u, error=%d", batch_id, transform_err);
    NvBufSurfaceDestroy(dst_surface);
    return NULL;
  }

  cudaStreamSynchronize(0);
  
  // ...existing CPU copy and JPEG encoding code...
  NvBufSurfaceParams *dst_params = &dst_surface->surfaceList[0];
  guint crop_w = dst_params->width;
  guint crop_h = dst_params->height;
  guint dst_pitch = dst_params->pitch;
  
  guint buffer_size = dst_pitch * crop_h;
  guchar *cpu_buffer = (guchar *)g_malloc(buffer_size);
  if (!cpu_buffer) {
    GST_ERROR("Failed to allocate CPU buffer (%u bytes)", buffer_size);
    NvBufSurfaceDestroy(dst_surface);
    return NULL;
  }
  
  gboolean data_copied = FALSE;
  
  if (dst_surface->memType == NVBUF_MEM_CUDA_UNIFIED && dst_params->dataPtr) {
    cudaError_t cuda_err = cudaMemcpy(cpu_buffer, dst_params->dataPtr, buffer_size, cudaMemcpyDeviceToHost);
    if (cuda_err == cudaSuccess) {
      data_copied = TRUE;
    } else {
      cudaDeviceSynchronize();
      memcpy(cpu_buffer, dst_params->dataPtr, buffer_size);
      data_copied = TRUE;
    }
  }
  
  if (!data_copied) {
    if (NvBufSurfaceMap(dst_surface, 0, 0, NVBUF_MAP_READ) == 0) {
      NvBufSurfaceSyncForCpu(dst_surface, 0, 0);
      
      guchar *mapped_data = NULL;
      if (dst_params->mappedAddr.addr[0]) {
        mapped_data = (guchar *)dst_params->mappedAddr.addr[0];
      } else if (dst_params->dataPtr) {
        mapped_data = (guchar *)dst_params->dataPtr;
      }
      
      if (mapped_data) {
        memcpy(cpu_buffer, mapped_data, buffer_size);
        data_copied = TRUE;
      }
      
      NvBufSurfaceUnMap(dst_surface, 0, 0);
    }
  }
  
  if (!data_copied && dst_params->dataPtr) {
    cudaError_t cuda_err = cudaMemcpy(cpu_buffer, dst_params->dataPtr, buffer_size, cudaMemcpyDeviceToHost);
    if (cuda_err == cudaSuccess) {
      data_copied = TRUE;
    }
  }
  
  if (!data_copied) {
    GST_ERROR("Failed to copy surface data to CPU");
    g_free(cpu_buffer);
    NvBufSurfaceDestroy(dst_surface);
    return NULL;
  }
  
  NvBufSurfaceDestroy(dst_surface);
  
  guint rgb_row_bytes = crop_w * 3;
  guchar *rgb_data = (guchar *)g_malloc(rgb_row_bytes * crop_h);
  if (!rgb_data) {
    GST_ERROR("Failed to allocate RGB buffer");
    g_free(cpu_buffer);
    return NULL;
  }
  
  for (guint y = 0; y < crop_h; y++) {
    guchar *src_row = cpu_buffer + y * dst_pitch;
    guchar *dst_row = rgb_data + y * rgb_row_bytes;
    
    for (guint x = 0; x < crop_w; x++) {
      dst_row[x * 3 + 0] = src_row[x * 4 + 0];
      dst_row[x * 3 + 1] = src_row[x * 4 + 1];
      dst_row[x * 3 + 2] = src_row[x * 4 + 2];
    }
  }
  
  g_free(cpu_buffer);
  
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  
  unsigned char *jpeg_buffer = NULL;
  unsigned long jpeg_size = 0;
  
  jpeg_mem_dest(&cinfo, &jpeg_buffer, &jpeg_size);
  
  cinfo.image_width = crop_w;
  cinfo.image_height = crop_h;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  
  jpeg_start_compress(&cinfo, TRUE);
  
  JSAMPROW row_pointer[1];
  while (cinfo.next_scanline < cinfo.image_height) {
    row_pointer[0] = &rgb_data[cinfo.next_scanline * rgb_row_bytes];
    jpeg_write_scanlines(&cinfo, row_pointer, 1);
  }
  
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  
  g_free(rgb_data);
  
  gchar *base64_image = g_base64_encode(jpeg_buffer, jpeg_size);
  
  free(jpeg_buffer);
  
  return base64_image;
}


gchar *
save_frame_to_jpeg(NvBufSurface *surface, NvDsFrameMeta *frame_meta,
                   const gchar *base_output_dir, gint quality)
{
  // ...existing validation code...
  if (!surface || !frame_meta || !base_output_dir) {
    GST_ERROR("Invalid parameters for save_frame_to_jpeg");
    return NULL;
  }

  if (surface->numFilled < 1 || !surface->surfaceList) {
    GST_ERROR("Surface has no data");
    return NULL;
  }

  guint batch_id = frame_meta->batch_id;
  NvBufSurfaceParams *src_params = &surface->surfaceList[batch_id];
  if (src_params->width == 0 || src_params->height == 0) {
    GST_ERROR("Invalid surface dimensions");
    return NULL;
  }

  guint source_id = frame_meta->source_id;
  guint frame_num = frame_meta->frame_num;
  gboolean infer_ok = frame_meta->bInferDone;

  gchar *rel_dir = g_strdup_printf("source_%u", source_id);
  gchar *abs_dir = g_strdup_printf("%s/%s", base_output_dir, rel_dir);

  if (!ensure_frame_save_directory(abs_dir)) {
    GST_ERROR("Failed to create directory: %s", abs_dir);
    g_free(rel_dir);
    g_free(abs_dir);
    return NULL;
  }

  GDateTime *now = g_date_time_new_now_local();
  gchar *timestamp = g_date_time_format(now, "%Y%m%d_%H%M%S");
  g_date_time_unref(now);

  gchar *filename = g_strdup_printf("frame_src%u_num%u_%d_%s.jpg",
      source_id, frame_num, infer_ok, timestamp);

  g_free(timestamp);

  gchar *relative_path = g_strdup_printf("%s/%s", rel_dir, filename);
  gchar *absolute_path = g_strdup_printf("%s/%s", abs_dir, filename);

  g_free(rel_dir);
  g_free(abs_dir);
  g_free(filename);

  // ...existing surface creation and transform code...
  NvBufSurface *dst_surface = NULL;
  NvBufSurfaceCreateParams create_params = {0};

  create_params.gpuId = surface->gpuId;
  create_params.width = src_params->width;
  create_params.height = src_params->height;
  create_params.isContiguous = 1;
  create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create_params.layout = NVBUF_LAYOUT_PITCH;

#ifdef __aarch64__
  create_params.memType = NVBUF_MEM_DEFAULT;
#else
  create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
#endif

  if (NvBufSurfaceCreate(&dst_surface, 1, &create_params) != 0) {
    GST_ERROR("Failed to create destination surface");
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  NvBufSurfTransformParams transform_params = {0};
  transform_params.transform_flag = NVBUFSURF_TRANSFORM_FILTER;
  transform_params.transform_filter = NvBufSurfTransformInter_Default;

  NvBufSurfTransformConfigParams config_params = {0};
  config_params.compute_mode = NvBufSurfTransformCompute_Default;
  config_params.gpu_id = surface->gpuId;
  config_params.cuda_stream = NULL;

  NvBufSurfaceParams orig_first = surface->surfaceList[0];
  guint orig_numFilled = surface->numFilled;
  guint orig_batchSize = surface->batchSize;

  if (batch_id > 0) {
    surface->surfaceList[0] = surface->surfaceList[batch_id];
  }
  surface->numFilled = 1;
  surface->batchSize = 1;

  if (NvBufSurfTransformSetSessionParams(&config_params) != 0) {
    GST_ERROR("Failed to set transform session params");
    NvBufSurfaceDestroy(dst_surface);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  NvBufSurfTransform_Error transform_err = NvBufSurfTransform(surface, dst_surface, &transform_params);

  surface->surfaceList[0] = orig_first;
  surface->numFilled = orig_numFilled;
  surface->batchSize = orig_batchSize;

  if (transform_err != NvBufSurfTransformError_Success) {
    GST_ERROR("Failed to transform surface for batch_id=%u, error=%d", batch_id, transform_err);
    NvBufSurfaceDestroy(dst_surface);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  cudaStreamSynchronize(0);

  // ...existing CPU copy and JPEG encoding code...
  NvBufSurfaceParams *dst_params = &dst_surface->surfaceList[0];
  guint width = dst_params->width;
  guint height = dst_params->height;
  guint pitch = dst_params->pitch;

  guint buffer_size = pitch * height;
  guchar *cpu_buffer = g_malloc(buffer_size);
  if (!cpu_buffer) {
    GST_ERROR("Failed to allocate CPU buffer");
    NvBufSurfaceDestroy(dst_surface);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  cudaError_t err = cudaMemcpy(cpu_buffer, dst_params->dataPtr, 
                                buffer_size, cudaMemcpyDeviceToHost);
  if (err != cudaSuccess) {
    GST_ERROR("cudaMemcpy failed: %s", cudaGetErrorString(err));
    g_free(cpu_buffer);
    NvBufSurfaceDestroy(dst_surface);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  NvBufSurfaceDestroy(dst_surface);

  guint rgb_stride = width * 3;
  guchar *rgb_data = g_malloc(rgb_stride * height);
  if (!rgb_data) {
    g_free(cpu_buffer);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  for (guint y = 0; y < height; y++) {
    guchar *src = cpu_buffer + y * pitch;
    guchar *dst = rgb_data + y * rgb_stride;
    for (guint x = 0; x < width; x++) {
      dst[x * 3 + 0] = src[x * 4 + 0];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }

  g_free(cpu_buffer);

  FILE *outfile = fopen(absolute_path, "wb");
  if (!outfile) {
    GST_ERROR("Failed to open file: %s", absolute_path);
    g_free(rgb_data);
    g_free(relative_path);
    g_free(absolute_path);
    return NULL;
  }

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, outfile);

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  JSAMPROW row[1];
  while (cinfo.next_scanline < cinfo.image_height) {
    row[0] = &rgb_data[cinfo.next_scanline * rgb_stride];
    jpeg_write_scanlines(&cinfo, row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  fclose(outfile);

  g_free(rgb_data);
  g_free(absolute_path);

  return relative_path;
}
