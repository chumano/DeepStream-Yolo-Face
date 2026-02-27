# DeepStream Meta

## Cấu trúc tổng quan

```
GstBuffer
 └── NvDsBatchMeta                    ← Meta của toàn bộ batch (nhiều nguồn)
      ├── frame_meta_list
      │    └── NvDsFrameMeta          ← Meta của từng frame
      │         ├── obj_meta_list
      │         │    └── NvDsObjectMeta   ← Meta của từng object được detect
      │         │         ├── rect_params              (bounding box)
      │         │         ├── mask_params              (segmentation mask)
      │         │         ├── text_params              (OSD label)
      │         │         ├── classifier_meta_list     (classification result)
      │         │         └── obj_user_meta_list       (custom user data)
      │         └── frame_user_meta_list   ← Custom data gắn vào frame
      └── batch_user_meta_list            ← Custom data gắn vào batch
```

### Giải thích các thành phần chính

| Struct | Mô tả |
|---|---|
| `NvDsBatchMeta` | Chứa meta của toàn batch, có thể gồm nhiều stream/source |
| `NvDsFrameMeta` | Meta của một frame từ một source cụ thể, gồm `source_id`, `frame_num`, `num_obj_meta` |
| `NvDsObjectMeta` | Meta của một object được detect: class, confidence, bbox, tracking ID |
| `NvDsClassifierMeta` | Kết quả phân loại (nếu có secondary GIE) |
| `NvDsUserMeta` | Container chứa dữ liệu tuỳ biến (custom), dùng ở batch/frame/object level |
| `NvDsInferTensorMeta` | Raw tensor output từ inference engine |
| `NvDsDisplayMeta` | Dữ liệu vẽ OSD: text, line, rect, circle |

---

# C++

## Hàm thường dùng (C++)

> **Headers cần include:**
> ```cpp
> #include <gst/gst.h>
> #include "gstnvdsmeta.h"
> #include "nvdsmeta.h"
> #include "nvds_infer_tensor_meta.h"   // NvDsInferTensorMeta
> #include "nvbufsurface.h"
> ```

### 1. Lấy `NvDsBatchMeta` từ `GstBuffer`

```cpp
static GstPadProbeReturn
on_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta)
        return GST_PAD_PROBE_OK;

    // xử lý batch_meta ...
    return GST_PAD_PROBE_OK;
}
```

### 2. Duyệt qua tất cả frame trong batch

```cpp
void iterate_frames(NvDsBatchMeta *batch_meta)
{
    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list;
         l_frame != nullptr; l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);

        guint source_id = frame_meta->source_id;   // ID stream
        guint frame_num = frame_meta->frame_num;   // số thứ tự frame
        guint num_obj   = frame_meta->num_obj_meta; // số object

        g_print("Source=%u, Frame=%u, Objects=%u\n",
                source_id, frame_num, num_obj);
    }
}
```

### 3. Duyệt qua tất cả object trong frame

```cpp
void iterate_objects(NvDsFrameMeta *frame_meta)
{
    for (NvDsMetaList *l_obj = frame_meta->obj_meta_list;
         l_obj != nullptr; l_obj = l_obj->next)
    {
        NvDsObjectMeta *obj_meta = (NvDsObjectMeta *)(l_obj->data);

        gint   class_id   = obj_meta->class_id;
        gchar *label      = obj_meta->obj_label;   // tên class
        gfloat confidence = obj_meta->confidence;
        guint64 track_id  = obj_meta->object_id;   // tracking ID

        // Bounding box (tọa độ ảnh gốc)
        NvOSD_RectParams &rect = obj_meta->rect_params;
        g_print("  [%d] %s conf=%.2f bbox=(%.0f,%.0f,%.0f,%.0f) id=%" G_GUINT64_FORMAT "\n",
                class_id, label, confidence,
                rect.left, rect.top, rect.width, rect.height,
                track_id);
    }
}
```

### 4. Sửa bounding box và màu hiển thị

```cpp
void modify_object_display(NvDsObjectMeta *obj_meta)
{
    NvOSD_RectParams &rect = obj_meta->rect_params;

    rect.left   = MAX(0.0f, rect.left  - 5.0f);   // mở rộng bbox
    rect.top    = MAX(0.0f, rect.top   - 5.0f);
    rect.width  += 10.0f;
    rect.height += 10.0f;

    // Màu viền (RGBA, 0.0–1.0)
    rect.border_color = {1.0, 0.0, 0.0, 1.0};     // đỏ
    rect.border_width = 2;

    // Màu nền có alpha
    rect.has_bg_color = 1;
    rect.bg_color     = {0.0, 0.0, 0.0, 0.3};     // đen mờ
}
```

### 5. Sửa label text hiển thị OSD

```cpp
void set_display_text(NvDsObjectMeta *obj_meta, const std::string &text)
{
    NvOSD_TextParams &txt = obj_meta->text_params;

    // Cấp phát và gán chuỗi (DeepStream giải phóng bộ nhớ sau)
    txt.display_text = g_strdup(text.c_str());

    txt.x_offset = (guint)obj_meta->rect_params.left;
    txt.y_offset = (guint)MAX(0.0f, obj_meta->rect_params.top - 20.0f);

    txt.font_params.font_name  = (gchar *)"Serif";
    txt.font_params.font_size  = 12;
    txt.font_params.font_color = {1.0, 1.0, 1.0, 1.0};  // trắng

    txt.set_bg_clr    = 1;
    txt.text_bg_clr   = {0.0, 0.0, 1.0, 0.6};           // nền xanh
}
```

### 6. Thêm `NvDsDisplayMeta` (vẽ thêm hình / text)

```cpp
void add_display_meta(NvDsBatchMeta *batch_meta, NvDsFrameMeta *frame_meta,
                      const std::string &text, guint x, guint y)
{
    NvDsDisplayMeta *display_meta =
        nvds_acquire_display_meta_from_pool(batch_meta);

    display_meta->num_labels = 1;
    NvOSD_TextParams &txt = display_meta->text_params[0];

    txt.display_text = g_strdup(text.c_str());
    txt.x_offset     = x;
    txt.y_offset     = y;

    txt.font_params.font_name  = (gchar *)"Serif";
    txt.font_params.font_size  = 14;
    txt.font_params.font_color = {1.0, 1.0, 0.0, 1.0};  // vàng
    txt.set_bg_clr = 0;

    nvds_add_display_meta_to_frame(frame_meta, display_meta);
}
```

---

## Hàm nâng cao (C++)

### 7. Đọc raw tensor output từ GIE (`NvDsInferTensorMeta`)

```cpp
#include "nvds_infer_tensor_meta.h"

void get_tensor_output(NvDsFrameMeta *frame_meta)
{
    for (NvDsMetaList *l_user = frame_meta->frame_user_meta_list;
         l_user != nullptr; l_user = l_user->next)
    {
        NvDsUserMeta *user_meta = (NvDsUserMeta *)(l_user->data);

        if (user_meta->base_meta.meta_type != NVDSINFER_TENSOR_OUTPUT_META)
            continue;

        NvDsInferTensorMeta *tensor_meta =
            (NvDsInferTensorMeta *)(user_meta->user_meta_data);

        for (guint i = 0; i < tensor_meta->num_output_layers; i++) {
            NvDsInferLayerInfo &layer = tensor_meta->output_layers_info[i];
            float *buf = (float *)(tensor_meta->out_buf_ptrs_dev[i]);

            // Tính tổng số phần tử từ dims
            int size = 1;
            for (int d = 0; d < layer.inferDims.numDims; d++)
                size *= layer.inferDims.d[d];

            g_print("Layer '%s': total_elements=%d\n",
                    layer.layerName, size);
            // buf[0..size-1] chứa dữ liệu tensor (trên device GPU)
        }
    }
}
```

### 8. Gắn custom user meta vào object (`obj_user_meta_list`)

```cpp
// Struct dữ liệu tuỳ biến
struct FaceEmbedding {
    float    embedding[512];
    guint64  track_id;
};

#define FACE_EMBEDDING_META_TYPE ((NvDsMetaType)(NVDS_START_USER_META + 100))

static gpointer copy_face_embedding(gpointer data, gpointer /*user_data*/)
{
    FaceEmbedding *src  = (FaceEmbedding *)data;
    FaceEmbedding *dest = new FaceEmbedding(*src);
    return dest;
}

static void release_face_embedding(gpointer data, gpointer /*user_data*/)
{
    delete (FaceEmbedding *)data;
}

void attach_embedding_to_object(NvDsBatchMeta *batch_meta,
                                NvDsObjectMeta *obj_meta,
                                const float *embedding, guint64 track_id)
{
    NvDsUserMeta *user_meta = nvds_acquire_user_meta_from_pool(batch_meta);

    FaceEmbedding *face_emb = new FaceEmbedding();
    memcpy(face_emb->embedding, embedding, sizeof(float) * 512);
    face_emb->track_id = track_id;

    user_meta->user_meta_data               = face_emb;
    user_meta->base_meta.meta_type          = FACE_EMBEDDING_META_TYPE;
    user_meta->base_meta.copy_func          = copy_face_embedding;
    user_meta->base_meta.release_func       = release_face_embedding;

    nvds_add_user_meta_to_obj(obj_meta, user_meta);
}

FaceEmbedding *read_embedding_from_object(NvDsObjectMeta *obj_meta)
{
    for (NvDsMetaList *l = obj_meta->obj_user_meta_list; l; l = l->next) {
        NvDsUserMeta *user_meta = (NvDsUserMeta *)(l->data);
        if (user_meta->base_meta.meta_type == FACE_EMBEDDING_META_TYPE)
            return (FaceEmbedding *)(user_meta->user_meta_data);
    }
    return nullptr;
}
```

### 9. Thêm / xoá object meta thủ công

```cpp
void add_object_meta(NvDsBatchMeta *batch_meta, NvDsFrameMeta *frame_meta,
                     float left, float top, float width, float height,
                     gint class_id, const char *label, float confidence)
{
    NvDsObjectMeta *obj_meta = nvds_acquire_obj_meta_from_pool(batch_meta);

    obj_meta->class_id   = class_id;
    obj_meta->confidence = confidence;
    obj_meta->object_id  = UNTRACKED_OBJECT_ID;
    g_strlcpy(obj_meta->obj_label, label, MAX_LABEL_SIZE);

    NvOSD_RectParams &rect = obj_meta->rect_params;
    rect.left   = left;
    rect.top    = top;
    rect.width  = width;
    rect.height = height;
    rect.border_color = {0.0, 1.0, 0.0, 1.0};
    rect.border_width = 2;

    // Lưu bbox gốc
    obj_meta->detector_bbox_info.org_bbox_coords = {left, top, width, height};

    nvds_add_obj_meta_to_frame(frame_meta, obj_meta, nullptr);
}

void remove_object_meta(NvDsFrameMeta *frame_meta, NvDsObjectMeta *obj_meta)
{
    nvds_remove_obj_meta_from_frame(frame_meta, obj_meta);
    nvds_destroy_obj_meta(obj_meta);
}
```

### 10. Gắn custom meta vào batch (`batch_user_meta_list`)

```cpp
void attach_batch_user_meta(NvDsBatchMeta *batch_meta,
                             const std::string &json_payload)
{
    NvDsUserMeta *user_meta = nvds_acquire_user_meta_from_pool(batch_meta);

    // Cấp phát bản sao chuỗi; release_func sẽ giải phóng
    char *buf = g_strdup(json_payload.c_str());

    user_meta->user_meta_data         = buf;
    user_meta->base_meta.meta_type    = NVDS_BATCH_GST_META;
    user_meta->base_meta.copy_func    = [](gpointer d, gpointer) -> gpointer {
        return g_strdup((char *)d);
    };
    user_meta->base_meta.release_func = [](gpointer d, gpointer) {
        g_free(d);
    };

    nvds_add_user_meta_to_batch(batch_meta, user_meta);
}
```

### 11. Lấy kết quả classifier (Secondary GIE)

```cpp
std::vector<std::pair<std::string, float>>
get_classifier_result(NvDsObjectMeta *obj_meta)
{
    std::vector<std::pair<std::string, float>> results;

    for (NvDsMetaList *l_cls = obj_meta->classifier_meta_list;
         l_cls != nullptr; l_cls = l_cls->next)
    {
        NvDsClassifierMeta *cls_meta = (NvDsClassifierMeta *)(l_cls->data);

        for (NvDsMetaList *l_label = cls_meta->label_info_list;
             l_label != nullptr; l_label = l_label->next)
        {
            NvDsLabelInfo *label_info = (NvDsLabelInfo *)(l_label->data);
            results.emplace_back(label_info->result_label,
                                 label_info->result_prob);
        }
    }
    return results;
}
```

---

## Pattern probe hoàn chỉnh (C++)

```cpp
#include <gst/gst.h>
#include "gstnvdsmeta.h"
#include "nvdsmeta.h"
#include <cstdio>

static GstPadProbeReturn
osd_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info,
                          gpointer user_data)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf)
        return GST_PAD_PROBE_OK;

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta)
        return GST_PAD_PROBE_OK;

    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list;
         l_frame != nullptr; l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);

        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list;
             l_obj != nullptr; l_obj = l_obj->next)
        {
            NvDsObjectMeta *obj_meta = (NvDsObjectMeta *)(l_obj->data);

            // --- xử lý object ở đây ---
            NvOSD_RectParams &rect = obj_meta->rect_params;
            g_print("[%u] class=%d conf=%.2f "
                    "bbox=(%.0f,%.0f,%.0f,%.0f)\n",
                    frame_meta->frame_num,
                    obj_meta->class_id,
                    obj_meta->confidence,
                    rect.left, rect.top, rect.width, rect.height);
        }
    }

    return GST_PAD_PROBE_OK;
}

// Đăng ký probe lên pad của element (ví dụ: nvosd)
// GstElement *nvosd = gst_bin_get_by_name(GST_BIN(pipeline), "nvosd");
// GstPad *osd_sink_pad = gst_element_get_static_pad(nvosd, "sink");
// gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
//                  osd_sink_pad_buffer_probe, nullptr, nullptr);
// gst_object_unref(osd_sink_pad);
```

---


# Python
## Hàm thường dùng (Python)

### 1. Lấy `NvDsBatchMeta` từ `GstBuffer`

```python
import pyds

def on_buffer(pad, info, u_data):
    gst_buffer = info.get_buffer()
    batch_meta = pyds.gst_buffer_get_nvds_batch_meta(hash(gst_buffer))
    return Gst.PadProbeReturn.OK
```

### 2. Duyệt qua tất cả frame trong batch

```python
def iterate_frames(batch_meta):
    l_frame = batch_meta.frame_meta_list
    while l_frame is not None:
        frame_meta = pyds.NvDsFrameMeta.cast(l_frame.data)

        source_id  = frame_meta.source_id    # ID của stream
        frame_num  = frame_meta.frame_num    # Số thứ tự frame
        num_obj    = frame_meta.num_obj_meta # Số object trong frame

        print(f"Source={source_id}, Frame={frame_num}, Objects={num_obj}")

        try:
            l_frame = l_frame.next
        except StopIteration:
            break
```

### 3. Duyệt qua tất cả object trong frame

```python
def iterate_objects(frame_meta):
    l_obj = frame_meta.obj_meta_list
    while l_obj is not None:
        obj_meta = pyds.NvDsObjectMeta.cast(l_obj.data)

        class_id    = obj_meta.class_id
        label       = obj_meta.obj_label       # tên class
        confidence  = obj_meta.confidence
        track_id    = obj_meta.object_id       # tracking ID (NVDC/ByteTrack)

        # Bounding box (ở tọa độ ảnh gốc)
        rect = obj_meta.rect_params
        left, top     = rect.left, rect.top
        width, height = rect.width, rect.height

        print(f"  [{class_id}] {label} conf={confidence:.2f} "
              f"bbox=({left:.0f},{top:.0f},{width:.0f},{height:.0f}) id={track_id}")

        try:
            l_obj = l_obj.next
        except StopIteration:
            break
```

### 4. Sửa bounding box và màu hiển thị

```python
def modify_object_display(obj_meta):
    rect = obj_meta.rect_params
    rect.left   = max(0, rect.left - 5)   # mở rộng bbox
    rect.top    = max(0, rect.top  - 5)
    rect.width  += 10
    rect.height += 10

    # Đổi màu viền (RGBA, 0.0–1.0)
    rect.border_color.set(1.0, 0.0, 0.0, 1.0)   # đỏ
    rect.border_width = 2

    # Màu nền (có alpha)
    rect.has_bg_color = 1
    rect.bg_color.set(0.0, 0.0, 0.0, 0.3)        # đen mờ
```

### 5. Sửa label text hiển thị OSD

```python
def set_display_text(obj_meta, text: str):
    py_object  = pyds.NvDsDisplayMeta    # chỉ cần với display_meta
    txt_params = obj_meta.text_params
    txt_params.display_text = text

    txt_params.x_offset = int(obj_meta.rect_params.left)
    txt_params.y_offset = max(0, int(obj_meta.rect_params.top) - 20)

    txt_params.font_params.font_name  = "Serif"
    txt_params.font_params.font_size  = 12
    txt_params.font_params.font_color.set(1.0, 1.0, 1.0, 1.0)  # trắng

    txt_params.set_bg_clr        = 1
    txt_params.text_bg_clr.set(0.0, 0.0, 1.0, 0.6)             # nền xanh
```

### 6. Thêm `NvDsDisplayMeta` (vẽ thêm hình / text)

```python
def add_display_meta(batch_meta, frame_meta, text: str, x: int, y: int):
    display_meta = pyds.nvds_acquire_display_meta_from_pool(batch_meta)
    display_meta.num_labels = 1

    txt = display_meta.text_params[0]
    txt.display_text = text
    txt.x_offset, txt.y_offset = x, y
    txt.font_params.font_name = "Serif"
    txt.font_params.font_size = 14
    txt.font_params.font_color.set(1.0, 1.0, 0.0, 1.0)  # vàng
    txt.set_bg_clr = 0

    pyds.nvds_add_display_meta_to_frame(frame_meta, display_meta)
```

---

## Hàm nâng cao

### 7. Đọc raw tensor output từ GIE (`NvDsInferTensorMeta`)

```python
import numpy as np
import ctypes

def get_tensor_output(frame_meta):
    """Lấy tensor output của primary GIE từ frame_user_meta_list."""
    l_user = frame_meta.frame_user_meta_list
    while l_user is not None:
        user_meta = pyds.NvDsUserMeta.cast(l_user.data)

        if user_meta.base_meta.meta_type == pyds.NvDsMetaType.NVDSINFER_TENSOR_OUTPUT_META:
            tensor_meta = pyds.NvDsInferTensorMeta.cast(user_meta.user_meta_data)

            # Duyệt qua từng output layer
            for i in range(tensor_meta.num_output_layers):
                layer = pyds.get_nvds_LayerInfo(tensor_meta, i)
                ptr   = ctypes.cast(pyds.get_ptr(layer.buffer), ctypes.POINTER(ctypes.c_float))
                dims  = [layer.dims.d[j] for j in range(layer.dims.numDims)]
                size  = 1
                for d in dims:
                    size *= d
                arr = np.array([ptr[k] for k in range(size)], dtype=np.float32).reshape(dims)
                print(f"Layer '{layer.layerName}': shape={arr.shape}")

        try:
            l_user = l_user.next
        except StopIteration:
            break
```

### 8. Gắn custom user meta vào object (`obj_user_meta_list`)

```python
# Định nghĩa struct tuỳ biến
class FaceEmbedding(ctypes.Structure):
    _fields_ = [("embedding", ctypes.c_float * 512),
                ("track_id",  ctypes.c_uint64)]

FACE_EMBEDDING_META_TYPE = pyds.get_first_meta_id() + 100   # ID không conflict

def copy_func(data, _):
    src  = ctypes.cast(data, ctypes.POINTER(FaceEmbedding))
    dest = FaceEmbedding()
    ctypes.memmove(ctypes.byref(dest), src, ctypes.sizeof(FaceEmbedding))
    return ctypes.addressof(dest)

def release_func(data, _):
    pass   # Python GC tự dọn nếu dùng ctypes

def attach_embedding_to_object(batch_meta, obj_meta, embedding: np.ndarray, track_id: int):
    user_meta = pyds.nvds_acquire_user_meta_from_pool(batch_meta)

    face_emb = FaceEmbedding()
    face_emb.embedding[:] = embedding.astype(np.float32).tolist()
    face_emb.track_id     = track_id

    user_meta.user_meta_data    = ctypes.addressof(face_emb)
    user_meta.base_meta.meta_type = FACE_EMBEDDING_META_TYPE
    user_meta.base_meta.copy_func    = pyds.copyfunc(copy_func)
    user_meta.base_meta.release_func = pyds.freefunc(release_func)

    pyds.nvds_add_user_meta_to_obj(obj_meta, user_meta)

def read_embedding_from_object(obj_meta) -> np.ndarray | None:
    l_user = obj_meta.obj_user_meta_list
    while l_user is not None:
        user_meta = pyds.NvDsUserMeta.cast(l_user.data)
        if user_meta.base_meta.meta_type == FACE_EMBEDDING_META_TYPE:
            face_emb = ctypes.cast(user_meta.user_meta_data, ctypes.POINTER(FaceEmbedding)).contents
            return np.array(face_emb.embedding[:])
        try:
            l_user = l_user.next
        except StopIteration:
            break
    return None
```

### 9. Thêm / xoá object meta thủ công

```python
def add_object_meta(batch_meta, frame_meta,
                    left, top, width, height,
                    class_id: int, label: str, confidence: float):
    """Thêm một object meta mới (pseudo-detection)."""
    obj_meta = pyds.nvds_acquire_obj_meta_from_pool(batch_meta)

    obj_meta.class_id   = class_id
    obj_meta.obj_label  = label
    obj_meta.confidence = confidence
    obj_meta.object_id  = pyds.UNTRACKED_OBJECT_ID

    rect = obj_meta.rect_params
    rect.left, rect.top     = left, top
    rect.width, rect.height = width, height
    rect.border_color.set(0.0, 1.0, 0.0, 1.0)
    rect.border_width = 2

    obj_meta.detector_bbox_info.org_bbox_coords.left   = left
    obj_meta.detector_bbox_info.org_bbox_coords.top    = top
    obj_meta.detector_bbox_info.org_bbox_coords.width  = width
    obj_meta.detector_bbox_info.org_bbox_coords.height = height

    pyds.nvds_add_obj_meta_to_frame(frame_meta, obj_meta, None)


def remove_object_meta(frame_meta, obj_meta):
    """Xoá một object meta khỏi frame."""
    pyds.nvds_remove_obj_meta_from_frame(frame_meta, obj_meta)
    pyds.nvds_destroy_obj_meta(obj_meta)
```

### 10. Gắn custom meta vào batch (`batch_user_meta_list`)

```python
def attach_batch_user_meta(batch_meta, payload: dict):
    """Gắn dict Python vào batch meta (dùng ctypes string buffer)."""
    import json
    data_bytes = json.dumps(payload).encode()
    buf        = ctypes.create_string_buffer(data_bytes)

    user_meta = pyds.nvds_acquire_user_meta_from_pool(batch_meta)
    user_meta.user_meta_data      = ctypes.addressof(buf)
    user_meta.base_meta.meta_type = pyds.NvDsMetaType.NVDS_BATCH_GST_META   # hoặc custom type

    pyds.nvds_add_user_meta_to_batch(batch_meta, user_meta)
```

### 11. Lấy kết quả classifier (Secondary GIE)

```python
def get_classifier_result(obj_meta) -> list[tuple[str, float]]:
    results = []
    l_cls = obj_meta.classifier_meta_list
    while l_cls is not None:
        cls_meta = pyds.NvDsClassifierMeta.cast(l_cls.data)

        l_label = cls_meta.label_info_list
        while l_label is not None:
            label_info = pyds.NvDsLabelInfo.cast(l_label.data)
            results.append((label_info.result_label, label_info.result_prob))
            try:
                l_label = l_label.next
            except StopIteration:
                break

        try:
            l_cls = l_cls.next
        except StopIteration:
            break
    return results
```

---

## Pattern probe hoàn chỉnh

```python
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst
import pyds

def osd_sink_pad_buffer_probe(pad, info, u_data):
    gst_buffer = info.get_buffer()
    if not gst_buffer:
        return Gst.PadProbeReturn.OK

    batch_meta = pyds.gst_buffer_get_nvds_batch_meta(hash(gst_buffer))

    l_frame = batch_meta.frame_meta_list
    while l_frame is not None:
        frame_meta = pyds.NvDsFrameMeta.cast(l_frame.data)

        l_obj = frame_meta.obj_meta_list
        while l_obj is not None:
            obj_meta = pyds.NvDsObjectMeta.cast(l_obj.data)

            # --- xử lý object ở đây ---
            rect = obj_meta.rect_params
            print(f"[{frame_meta.frame_num}] class={obj_meta.class_id} "
                  f"conf={obj_meta.confidence:.2f} "
                  f"bbox=({rect.left:.0f},{rect.top:.0f},"
                  f"{rect.width:.0f},{rect.height:.0f})")

            try:
                l_obj = l_obj.next
            except StopIteration:
                break

        try:
            l_frame = l_frame.next
        except StopIteration:
            break

    return Gst.PadProbeReturn.OK


# Đăng ký probe lên pad của element (ví dụ: nvosd)
nvosd = pipeline.get_by_name("nvosd")
osdsinkpad = nvosd.get_static_pad("sink")
osdsinkpad.add_probe(Gst.PadProbeType.BUFFER, osd_sink_pad_buffer_probe, 0)
```

---
