# Debugging Surface Contents in gdb
When DONT use capfilter so format is NV12_709, the surface format is NV12_709

add breakpoint in gdb to inspect surface contents when segmentation fault occurs in image_processing.c in gdb:
`break image_processing.c:313`

```bash
Thread 10 "queue_app:src" hit Breakpoint 1, save_frame_to_jpeg (surface=0x7fff3c0043e0, frame_meta=0x7fff2c0120d0, base_output_dir=0x555555810aa0 "/app/outputs/frames", quality=60) at modules/image_processing.c:313
313       if (!surface || !frame_meta || !base_output_dir) {

(gdb) print *surface
$1 = {gpuId = 0, batchSize = 2, numFilled = 2, isContiguous = false, memType = NVBUF_MEM_CUDA_DEVICE, 
  surfaceList = 0x7fff3c3138f0, _reserved = {0x0, 0x0, 0x0, 0x0}}

(gdb) print surface->surfaceList[0]
$2 = {width = 1920, height = 1080, pitch = 2048, colorFormat = NVBUF_COLOR_FORMAT_NV12_709, layout = NVBUF_LAYOUT_PITCH, 
  bufferDesc = 65346, dataSize = 3317760, dataPtr = 0x60de00000, planeParams = {num_planes = 2, width = {1920, 960, 0, 0},  
    height = {1080, 540, 0, 0}, pitch = {2048, 2048, 0, 0}, offset = {0, 2211840, 0, 0}, psize = {2211840, 1105920, 0, 0},  
    bytesPerPix = {1, 2, 0, 0}, _reserved = {0x0 <repeats 16 times>}}, mappedAddr = {addr = {0x0, 0x0, 0x0, 0x0},
    eglImage = 0x0, _reserved = {0x0, 0x0, 0x0, 0x0}}, paramex = 0x0, _reserved = {0x0, 0x0, 0x0}}

(gdb) print surface->surfaceList[1]
$3 = {width = 1920, height = 1080, pitch = 2048, colorFormat = NVBUF_COLOR_FORMAT_NV12_709, layout = NVBUF_LAYOUT_PITCH, 
  bufferDesc = 65339, dataSize = 3317760, dataPtr = 0x60c200000, planeParams = {num_planes = 2, width = {1920, 960, 0, 0},  
    height = {1080, 540, 0, 0}, pitch = {2048, 2048, 0, 0}, offset = {0, 2211840, 0, 0}, psize = {2211840, 1105920, 0, 0},  
    bytesPerPix = {1, 2, 0, 0}, _reserved = {0x0 <repeats 16 times>}}, mappedAddr = {addr = {0x0, 0x0, 0x0, 0x0},
    eglImage = 0x0, _reserved = {0x0, 0x0, 0x0, 0x0}}, paramex = 0x0, _reserved = {0x0, 0x0, 0x0}}

(gdb) print surface->surfaceList[1]->planeParams
$4 = {num_planes = 2, width = {1920, 960, 0, 0}, height = {1080, 540, 0, 0}, pitch = {2048, 2048, 0, 0}, 
  offset = {0, 2211840, 0, 0}, psize = {2211840, 1105920, 0, 0}, bytesPerPix = {1, 2, 0, 0}, _reserved = {
    0x0 <repeats 16 times>}}
```
## Explanation of surface contents

The `surface` structure represents a batch of image frames in GPU memory, typically used in DeepStream pipelines for efficient video processing. Here’s a breakdown of the key fields and their meanings:

- **gpuId = 0**: The surface is allocated on GPU 0.
- **batchSize = 2**: The surface contains 2 frames (batched processing).
- **numFilled = 2**: Both slots in the batch are filled with valid frames.
- **isContiguous = false**: The memory for the batch is not contiguous.
- **memType = NVBUF_MEM_CUDA_DEVICE**: The memory is allocated on the CUDA device (GPU).
- **surfaceList**: An array holding metadata for each frame in the batch.

Each entry in `surfaceList` (e.g., `surface->surfaceList[0]` and `surface->surfaceList[1]`) describes an individual frame:

- **width = 1920, height = 1080**: Frame resolution is 1920x1080 pixels.
- **pitch = 2048**: The number of bytes per row in memory (may be greater than width for alignment).
- **colorFormat = NVBUF_COLOR_FORMAT_NV12_709**: The frame uses the NV12 color format (YUV 4:2:0) with BT.709 color space.
- **layout = NVBUF_LAYOUT_PITCH**: The frame is stored in pitch-linear layout.
- **bufferDesc**: A descriptor/handle for the buffer (unique per frame).
- **dataSize = 3317760**: Total size of the frame data in bytes.
- **dataPtr**: GPU memory address where the frame data starts.
- **planeParams**: Describes the planes in the image (for NV12, there are 2 planes: Y and UV).
  - **num_planes = 2**: Y and UV planes.
  - **width, height, pitch, offset, psize, bytesPerPix**: Arrays describing each plane’s dimensions, memory layout, and size.
    - **pitch**: Number of bytes in a row of the image in memory for each plane. This may be greater than the image width due to alignment for efficient GPU access.
    - **offset**: The starting byte position of each plane within the buffer. For multi-plane formats, the first plane starts at 0, and subsequent planes start at the offset equal to the sum of previous plane sizes.
    - **psize**: The size in bytes of each plane. For example, in NV12, the Y plane size is width × height, and the UV plane size is width × height / 2.
    - **bytesPerPix**: Number of bytes used to represent a single pixel in each plane. For NV12, the Y plane uses 1 byte per pixel, and the UV plane uses 2 bytes per 2 pixels (i.e., 1 byte per pixel for Y, 2 bytes for each pair of UV samples).
    
- **mappedAddr**: Addresses for mapped memory (all 0 here, meaning not mapped to CPU).
- **eglImage, paramex, _reserved**: Reserved or unused fields.

This structure is crucial for debugging image processing issues, as it allows you to inspect the properties and memory layout of each frame being processed. If a segmentation fault occurs, checking these fields can help identify issues such as invalid pointers, incorrect batch sizes, or memory misalignment.

## Read frame to cpu memory for debugging

To inspect pixel data, the GPU buffer must be mapped to CPU-accessible memory using DeepStream's
`NvBufSurfaceMap` / `NvBufSurfaceSyncForCpu` API. The mapping is per-frame (batch index) and
per-plane:

```
NvBufSurfaceMap(surface, batch_idx, plane_idx, NVBUF_MAP_READ)
NvBufSurfaceSyncForCpu(surface, batch_idx, plane_idx)
// → surface->surfaceList[batch_idx].mappedAddr.addr[plane_idx] is now valid CPU ptr
NvBufSurfaceUnMap(surface, batch_idx, plane_idx)
```

### NV12 memory layout

For the surface above (1920×1080, pitch=2048):

```
Plane 0 – Y (luma)
  rows   : 1080
  columns: 1920 pixels, 1 byte each
  pitch  : 2048 bytes  → 128 bytes of padding per row (for GPU alignment)
  psize  : 2048 × 1080 = 2,211,840 bytes

Plane 1 – UV (chroma, interleaved)
  rows   : 540  (half height)
  columns: 960 U/V pairs, 2 bytes each (U then V, packed)
  pitch  : 2048 bytes
  psize  : 2048 × 540 = 1,105,920 bytes

Total dataSize = 2,211,840 + 1,105,920 = 3,317,760 bytes  ✓
```

To read pixel (x, y) from the Y plane:
```
uint8_t luma = y_ptr[y * pitch + x];          // pitch=2048, not width=1920
```

To read the U and V components at the same pixel (sampled at half resolution):
```
uint8_t u = uv_ptr[(y/2) * pitch + (x & ~1)];     // even byte = U
uint8_t v = uv_ptr[(y/2) * pitch + (x & ~1) + 1]; // odd  byte = V
```

### Code: map, read, and dump a frame

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nvbufsurface.h"   // NvBufSurface*, NvBufSurfaceMap, etc.

/* -----------------------------------------------------------------------
 * dump_surface_frame_raw
 *
 * Maps frame `batch_idx` to CPU memory and writes two raw binary files:
 *   <dir>/frame_<batch_idx>_Y.raw   – luma plane  (width × height bytes)
 *   <dir>/frame_<batch_idx>_UV.raw  – chroma plane (width × height/2 bytes)
 *
 * The files can be opened with ffplay:
 *   ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 Y.raw
 * ----------------------------------------------------------------------- */
static void dump_surface_frame_raw(NvBufSurface *surface, guint batch_idx,
                                   const char *out_dir)
{
    if (!surface || batch_idx >= surface->numFilled) {
        g_printerr("dump_surface_frame_raw: invalid args\n");
        return;
    }

    NvBufSurfaceParams *params = &surface->surfaceList[batch_idx];
    guint width  = params->width;
    guint height = params->height;
    guint pitch  = params->pitch;            /* bytes per row (≥ width) */
    guint num_planes = params->planeParams.num_planes;  /* 2 for NV12 */

    /* --- map all planes read-only ---------------------------------------- */
    for (guint p = 0; p < num_planes; p++) {
        if (NvBufSurfaceMap(surface, (gint)batch_idx, (gint)p,
                            NVBUF_MAP_READ) != 0) {
            g_printerr("NvBufSurfaceMap failed for plane %u\n", p);
            return;
        }
    }

    /* sync GPU → CPU for all planes */
    if (NvBufSurfaceSyncForCpu(surface, (gint)batch_idx, -1) != 0) {
        g_printerr("NvBufSurfaceSyncForCpu failed\n");
        goto unmap;
    }

    /* --- Plane 0: Y luma -------------------------------------------------- */
    {
        uint8_t *y_ptr = (uint8_t *)params->mappedAddr.addr[0];
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%u_Y.raw", out_dir, batch_idx);
        FILE *fp = fopen(path, "wb");
        if (!fp) { perror("fopen Y"); goto unmap; }

        /* write row-by-row to strip the pitch padding */
        for (guint row = 0; row < height; row++) {
            fwrite(y_ptr + row * pitch, 1, width, fp);
        }
        fclose(fp);
        g_print("Wrote Y plane  → %s  (%u × %u)\n", path, width, height);
    }

    /* --- Plane 1: UV chroma (interleaved) --------------------------------- */
    {
        uint8_t *uv_ptr = (uint8_t *)params->mappedAddr.addr[1];
        guint uv_height = height / 2;       /* NV12: half as many UV rows  */
        guint uv_width  = width;            /* same row width (U+V packed) */

        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%u_UV.raw", out_dir, batch_idx);
        FILE *fp = fopen(path, "wb");
        if (!fp) { perror("fopen UV"); goto unmap; }

        for (guint row = 0; row < uv_height; row++) {
            fwrite(uv_ptr + row * pitch, 1, uv_width, fp);
        }
        fclose(fp);
        g_print("Wrote UV plane → %s  (%u × %u)\n", path, uv_width, uv_height);
    }

unmap:
    for (guint p = 0; p < num_planes; p++) {
        NvBufSurfaceUnMap(surface, (gint)batch_idx, (gint)p);
    }
}
```

### Code: convert NV12 → RGB and save as PPM (no extra dependencies)

PPM is a trivial uncompressed RGB format readable by most image viewers.

```c
/* -----------------------------------------------------------------------
 * nv12_pixel_to_rgb
 *
 * BT.709 limited-range YCbCr → RGB (clamp to [0,255]).
 * Y  ∈ [16..235],  Cb/Cr ∈ [16..240]
 * ----------------------------------------------------------------------- */
static void nv12_pixel_to_rgb(uint8_t Y, uint8_t U, uint8_t V,
                               uint8_t *r, uint8_t *g, uint8_t *b)
{
    int y  = (int)Y  - 16;
    int cb = (int)U  - 128;
    int cr = (int)V  - 128;

    /* BT.709 coefficients scaled by 1024 */
    int ri = (1192 * y              + 1836 * cr) >> 10;
    int gi = (1192 * y -  218 * cb -  547 * cr) >> 10;
    int bi = (1192 * y + 2166 * cb             ) >> 10;

    *r = (uint8_t)(ri < 0 ? 0 : ri > 255 ? 255 : ri);
    *g = (uint8_t)(gi < 0 ? 0 : gi > 255 ? 255 : gi);
    *b = (uint8_t)(bi < 0 ? 0 : bi > 255 ? 255 : bi);
}

/* -----------------------------------------------------------------------
 * dump_surface_frame_ppm
 *
 * Maps frame `batch_idx`, converts NV12 → RGB, writes a PPM file:
 *   <dir>/frame_<batch_idx>.ppm
 * ----------------------------------------------------------------------- */
static void dump_surface_frame_ppm(NvBufSurface *surface, guint batch_idx,
                                   const char *out_dir)
{
    if (!surface || batch_idx >= surface->numFilled) return;

    NvBufSurfaceParams *params = &surface->surfaceList[batch_idx];
    guint width  = params->width;
    guint height = params->height;
    guint pitch  = params->pitch;

    /* map both planes */
    for (guint p = 0; p < 2; p++) {
        if (NvBufSurfaceMap(surface, (gint)batch_idx, (gint)p,
                            NVBUF_MAP_READ) != 0) {
            g_printerr("NvBufSurfaceMap failed plane %u\n", p);
            return;
        }
    }
    if (NvBufSurfaceSyncForCpu(surface, (gint)batch_idx, -1) != 0) {
        g_printerr("NvBufSurfaceSyncForCpu failed\n");
        goto unmap_ppm;
    }

    {
        uint8_t *y_ptr  = (uint8_t *)params->mappedAddr.addr[0];
        uint8_t *uv_ptr = (uint8_t *)params->mappedAddr.addr[1];

        /* allocate RGB row buffer */
        uint8_t *rgb_row = (uint8_t *)malloc(width * 3);
        if (!rgb_row) { g_printerr("malloc failed\n"); goto unmap_ppm; }

        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%u.ppm", out_dir, batch_idx);
        FILE *fp = fopen(path, "wb");
        if (!fp) { perror("fopen PPM"); free(rgb_row); goto unmap_ppm; }

        /* PPM header */
        fprintf(fp, "P6\n%u %u\n255\n", width, height);

        for (guint y = 0; y < height; y++) {
            uint8_t *y_row  = y_ptr  + y       * pitch;
            uint8_t *uv_row = uv_ptr + (y / 2) * pitch;  /* shared UV row */

            for (guint x = 0; x < width; x++) {
                uint8_t Y_val = y_row[x];
                /* UV interleaved: U at even index, V at odd index */
                uint8_t U_val = uv_row[(x & ~1U)];        /* floor to even */
                uint8_t V_val = uv_row[(x & ~1U) + 1];

                nv12_pixel_to_rgb(Y_val, U_val, V_val,
                                  &rgb_row[x*3 + 0],
                                  &rgb_row[x*3 + 1],
                                  &rgb_row[x*3 + 2]);
            }
            fwrite(rgb_row, 3, width, fp);
        }
        fclose(fp);
        free(rgb_row);
        g_print("Wrote PPM → %s  (%u × %u)\n", path, width, height);
    }

unmap_ppm:
    for (guint p = 0; p < 2; p++) {
        NvBufSurfaceUnMap(surface, (gint)batch_idx, (gint)p);
    }
}
```

### Usage in a probe callback

Insert the dump calls in a pad probe before the crash site:

```c
static GstPadProbeReturn before_crash_probe(GstPad *pad,
                                             GstPadProbeInfo *info,
                                             gpointer user_data)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;

    NvBufSurface *surface = (NvBufSurface *)map.data;

    /* dump every filled frame before processing */
    for (guint i = 0; i < surface->numFilled; i++) {
        dump_surface_frame_ppm(surface, i, "/app/outputs/debug");
        /* or: dump_surface_frame_raw(surface, i, "/app/outputs/debug"); */
    }

    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
}

/* attach probe: */
GstPad *sink_pad = gst_element_get_static_pad(some_element, "sink");
gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                  before_crash_probe, NULL, NULL);
gst_object_unref(sink_pad);
```

### Verifying output with ffplay / ImageMagick

```bash
# Play raw NV12 file (Y + UV concatenated)
cat frame_0_Y.raw frame_0_UV.raw > frame_0.nv12
ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 frame_0.nv12

# View PPM directly
display frame_0.ppm          # ImageMagick
eog    frame_0.ppm            # GNOME image viewer
```
