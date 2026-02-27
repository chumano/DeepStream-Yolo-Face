# Debugging Surface Contents in gdb
When use capfilter `video/x-raw(memory:NVMM), format=RGBA`, the surface color format is RGBA, and the memory layout is pitch-linear. The `surface` structure contains metadata about the batch of frames being processed, including dimensions, color format, memory type, and pointers to the actual image data in GPU memory.

add breakpoint in gdb to inspect surface contents when segmentation fault occurs in image_processing.c in gdb:
`break image_processing.c:313`

```bash
Thread 10 "queue_app:src" hit Breakpoint 1, save_frame_to_jpeg (surface=0x7fff64000bf0, frame_meta=0x7fff640071f0,
 base_output_dir=0x555555810aa0 "/app/outputs/frames", quality=60) at modules/image_processing.c:313
313       if (!surface || !frame_meta || !base_output_dir) {
(gdb) print *surface
$1 = {gpuId = 0, batchSize = 2, numFilled = 2, isContiguous = false, memType = NVBUF_MEM_CUDA_DEVICE, 
  surfaceList = 0x7fff64000c40, _reserved = {0x0, 0x0, 0x0, 0x0}}
(gdb) print surface->surfaceList[0]
$2 = {width = 1920, height = 1080, pitch = 7680, colorFormat = NVBUF_COLOR_FORMAT_RGBA, 
  layout = NVBUF_LAYOUT_PITCH, bufferDesc = 65347, dataSize = 8294400, dataPtr = 0x612c00000, planeParams = {     
    num_planes = 1, width = {1920, 0, 0, 0}, height = {1080, 0, 0, 0}, pitch = {7680, 0, 0, 0}, offset = {0, 0,   
      0, 0}, psize = {8294400, 0, 0, 0}, bytesPerPix = {4, 0, 0, 0}, _reserved = {0x0 <repeats 16 times>}},       
  mappedAddr = {addr = {0x0, 0x0, 0x0, 0x0}, eglImage = 0x0, _reserved = {0x0, 0x0, 0x0, 0x0}}, paramex = 0x0,    
  _reserved = {0x0, 0x0, 0x0}}
(gdb) print surface->surfaceList[1]
$3 = {width = 1920, height = 1080, pitch = 7680, colorFormat = NVBUF_COLOR_FORMAT_RGBA, 
  layout = NVBUF_LAYOUT_PITCH, bufferDesc = 65348, dataSize = 8294400, dataPtr = 0x613400000, planeParams = {     
    num_planes = 1, width = {1920, 0, 0, 0}, height = {1080, 0, 0, 0}, pitch = {7680, 0, 0, 0}, offset = {0, 0,   
      0, 0}, psize = {8294400, 0, 0, 0}, bytesPerPix = {4, 0, 0, 0}, _reserved = {0x0 <repeats 16 times>}},       
  mappedAddr = {addr = {0x0, 0x0, 0x0, 0x0}, eglImage = 0x0, _reserved = {0x0, 0x0, 0x0, 0x0}}, paramex = 0x0,    
  _reserved = {0x0, 0x0, 0x0}}
(gdb) print surface->surfaceList[1]->planeParams
$4 = {num_planes = 1, width = {1920, 0, 0, 0}, height = {1080, 0, 0, 0}, pitch = {7680, 0, 0, 0}, offset = {0, 
    0, 0, 0}, psize = {8294400, 0, 0, 0}, bytesPerPix = {4, 0, 0, 0}, _reserved = {0x0 <repeats 16 times>}} 
```

## Explanation of surface contents

The `surface` structure represents a batch of image frames in GPU memory in RGBA format. Key differences from NV12:

- **gpuId = 0**: Allocated on GPU 0.
- **batchSize = 2 / numFilled = 2**: Two frames in the batch, both valid.
- **isContiguous = false**: Frames are not in contiguous GPU memory (separate `dataPtr` per frame).
- **memType = NVBUF_MEM_CUDA_DEVICE**: Memory lives on the CUDA device.
- **colorFormat = NVBUF_COLOR_FORMAT_RGBA**: Each pixel is 4 bytes: R, G, B, A (one byte each).
- **num_planes = 1**: RGBA is a packed single-plane format — no separate chroma plane like NV12.
- **width = 1920, height = 1080**.
- **pitch = 7680**: Bytes per row = 1920 pixels × 4 bytes/pixel = 7680. No padding here; width × bpp already satisfies GPU alignment.
- **bytesPerPix = {4}**: 4 bytes per pixel (RGBA).
- **psize = {8294400}**: 7680 × 1080 = 8,294,400 bytes — entire frame in one plane.
- **dataSize = 8294400**: Matches `psize` since there is only one plane.
- **dataPtr**: GPU virtual address of the frame buffer (different per frame: `0x612c00000` and `0x613400000`).
- **mappedAddr = {0x0 …}**: Not yet mapped to CPU address space.

### Memory layout

```
Plane 0 – RGBA (packed)
  rows      : 1080
  columns   : 1920 pixels × 4 bytes = 7680 bytes
  pitch     : 7680 bytes  (no padding — width × 4 is already GPU-aligned)
  psize     : 7680 × 1080 = 8,294,400 bytes

Pixel layout within a row (byte offsets):
  byte 0 : R
  byte 1 : G
  byte 2 : B
  byte 3 : A
  byte 4 : R  ← next pixel
  ...
```

To read pixel `(x, y)`:
```c
uint8_t *base = (uint8_t *)params->mappedAddr.addr[0];
uint8_t *px   = base + y * pitch + x * 4;
uint8_t r = px[0], g = px[1], b = px[2], a = px[3];
```

## Read frame to cpu memory for debugging

Mapping is simpler than NV12 because there is only one plane:

```
NvBufSurfaceMap(surface, batch_idx, 0, NVBUF_MAP_READ)
NvBufSurfaceSyncForCpu(surface, batch_idx, 0)
// → surface->surfaceList[batch_idx].mappedAddr.addr[0] is a valid CPU ptr
NvBufSurfaceUnMap(surface, batch_idx, 0)
```

### Code: map and dump raw RGBA bytes

```c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "nvbufsurface.h"

/* -----------------------------------------------------------------------
 * dump_surface_frame_rgba_raw
 *
 * Maps frame `batch_idx` to CPU memory and writes a raw binary file:
 *   <dir>/frame_<batch_idx>_RGBA.raw  – packed RGBA, width×height×4 bytes
 *
 * View with ffplay:
 *   ffplay -f rawvideo -pixel_format rgba -video_size 1920x1080 frame_0_RGBA.raw
 * ----------------------------------------------------------------------- */
static void dump_surface_frame_rgba_raw(NvBufSurface *surface, guint batch_idx,
                                        const char *out_dir)
{
    if (!surface || batch_idx >= surface->numFilled) {
        g_printerr("dump_surface_frame_rgba_raw: invalid args\n");
        return;
    }

    NvBufSurfaceParams *params = &surface->surfaceList[batch_idx];
    guint width  = params->width;
    guint height = params->height;
    guint pitch  = params->pitch;   /* bytes per row – equals width×4 here */

    if (NvBufSurfaceMap(surface, (gint)batch_idx, 0, NVBUF_MAP_READ) != 0) {
        g_printerr("NvBufSurfaceMap failed\n");
        return;
    }
    if (NvBufSurfaceSyncForCpu(surface, (gint)batch_idx, 0) != 0) {
        g_printerr("NvBufSurfaceSyncForCpu failed\n");
        goto unmap;
    }

    {
        uint8_t *rgba_ptr = (uint8_t *)params->mappedAddr.addr[0];

        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%u_RGBA.raw", out_dir, batch_idx);
        FILE *fp = fopen(path, "wb");
        if (!fp) { perror("fopen RGBA"); goto unmap; }

        /* strip pitch padding (none here, but write row-by-row for safety) */
        for (guint row = 0; row < height; row++) {
            fwrite(rgba_ptr + row * pitch, 4, width, fp);
        }
        fclose(fp);
        g_print("Wrote RGBA raw → %s  (%u × %u, %u bytes)\n",
                path, width, height, width * height * 4);
    }

unmap:
    NvBufSurfaceUnMap(surface, (gint)batch_idx, 0);
}
```

### Code: save frame as PPM (no external library)

RGBA → RGB: simply discard the alpha channel.

```c
/* -----------------------------------------------------------------------
 * dump_surface_frame_rgba_ppm
 *
 * Maps frame `batch_idx`, drops the alpha channel, writes:
 *   <dir>/frame_<batch_idx>.ppm
 * ----------------------------------------------------------------------- */
static void dump_surface_frame_rgba_ppm(NvBufSurface *surface, guint batch_idx,
                                        const char *out_dir)
{
    if (!surface || batch_idx >= surface->numFilled) return;

    NvBufSurfaceParams *params = &surface->surfaceList[batch_idx];
    guint width  = params->width;
    guint height = params->height;
    guint pitch  = params->pitch;

    if (NvBufSurfaceMap(surface, (gint)batch_idx, 0, NVBUF_MAP_READ) != 0) {
        g_printerr("NvBufSurfaceMap failed\n");
        return;
    }
    if (NvBufSurfaceSyncForCpu(surface, (gint)batch_idx, 0) != 0) {
        g_printerr("NvBufSurfaceSyncForCpu failed\n");
        goto unmap_ppm;
    }

    {
        uint8_t *rgba_ptr = (uint8_t *)params->mappedAddr.addr[0];
        uint8_t *rgb_row  = (uint8_t *)malloc(width * 3);
        if (!rgb_row) { g_printerr("malloc failed\n"); goto unmap_ppm; }

        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%u.ppm", out_dir, batch_idx);
        FILE *fp = fopen(path, "wb");
        if (!fp) { perror("fopen PPM"); free(rgb_row); goto unmap_ppm; }

        fprintf(fp, "P6\n%u %u\n255\n", width, height);

        for (guint y = 0; y < height; y++) {
            uint8_t *src = rgba_ptr + y * pitch;
            for (guint x = 0; x < width; x++) {
                rgb_row[x * 3 + 0] = src[x * 4 + 0];  /* R */
                rgb_row[x * 3 + 1] = src[x * 4 + 1];  /* G */
                rgb_row[x * 3 + 2] = src[x * 4 + 2];  /* B */
                /* alpha src[x*4+3] discarded */
            }
            fwrite(rgb_row, 3, width, fp);
        }
        fclose(fp);
        free(rgb_row);
        g_print("Wrote PPM → %s  (%u × %u)\n", path, width, height);
    }

unmap_ppm:
    NvBufSurfaceUnMap(surface, (gint)batch_idx, 0);
}
```

### Code: read and inspect individual pixels

```c
static void inspect_surface_pixels(NvBufSurface *surface, guint batch_idx)
{
    NvBufSurfaceParams *params = &surface->surfaceList[batch_idx];
    guint pitch = params->pitch;

    if (NvBufSurfaceMap(surface, (gint)batch_idx, 0, NVBUF_MAP_READ) != 0) return;
    NvBufSurfaceSyncForCpu(surface, (gint)batch_idx, 0);

    uint8_t *base = (uint8_t *)params->mappedAddr.addr[0];

    /* sample the four corners */
    guint W = params->width - 1, H = params->height - 1;
    guint corners[4][2] = {{0,0}, {W,0}, {0,H}, {W,H}};
    const char *labels[] = {"top-left", "top-right", "bottom-left", "bottom-right"};

    for (int i = 0; i < 4; i++) {
        guint x = corners[i][0], y = corners[i][1];
        uint8_t *px = base + y * pitch + x * 4;
        g_print("frame[%u] %s (%u,%u)  R=%3u G=%3u B=%3u A=%3u\n",
                batch_idx, labels[i], x, y, px[0], px[1], px[2], px[3]);
    }

    NvBufSurfaceUnMap(surface, (gint)batch_idx, 0);
}
```

### Usage in a probe callback

```c
static GstPadProbeReturn before_crash_probe(GstPad *pad,
                                             GstPadProbeInfo *info,
                                             gpointer user_data)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;

    NvBufSurface *surface = (NvBufSurface *)map.data;

    for (guint i = 0; i < surface->numFilled; i++) {
        dump_surface_frame_rgba_ppm(surface, i, "/app/outputs/debug");
        inspect_surface_pixels(surface, i);
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

### Verifying output

```bash
# View raw RGBA file
ffplay -f rawvideo -pixel_format rgba -video_size 1920x1080 frame_0_RGBA.raw

# View PPM
display frame_0.ppm          # ImageMagick
eog    frame_0.ppm            # GNOME image viewer

# Convert PPM → JPEG with ImageMagick
convert frame_0.ppm frame_0.jpg
```
