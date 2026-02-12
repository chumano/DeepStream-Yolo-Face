import cv2
import numpy as np
import tritonclient.http as httpclient
import sys
import os
import time

# Usage: python triton/detect_face.py [image_path]
default_image_path = "images/faces.png"

def preprocess_image(image_path, input_shape):
    img = cv2.imread(image_path)
    if img is None:
        raise ValueError(f"Image not found: {image_path}")
    img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    img_resized = cv2.resize(img_rgb, (input_shape[3], input_shape[2]))
    img_transposed = np.transpose(img_resized, (2, 0, 1))
    img_normalized = img_transposed.astype(np.float32) / 255.0
    img_input = np.expand_dims(img_normalized, axis=0)
    return img_input, img

def clamp(val, minVal, maxVal):
    return max(minVal, min(maxVal, val))

def postprocess(result, orig_img, net_w, net_h, conf_thresh=0.4, nms_thresh=0.45):
    # result shape: (1, N, 20)
    detections = result[0]
    h0, w0 = orig_img.shape[:2]
    scale_w, scale_h = w0 / net_w, h0 / net_h
    boxes = []
    for det in detections:
        conf = det[4]
        if conf < conf_thresh:
            continue
        x1, y1, x2, y2 = det[0:4]
        x1 = clamp(x1, 0, net_w) * scale_w
        y1 = clamp(y1, 0, net_h) * scale_h
        x2 = clamp(x2, 0, net_w) * scale_w
        y2 = clamp(y2, 0, net_h) * scale_h
        w = clamp(x2 - x1, 0, net_w) * scale_w
        h = clamp(y2 - y1, 0, net_h) * scale_h
        if w < 1 or h < 1:
            continue
        landmarks = []
        for p in range(5):
            lx = clamp(det[5 + p * 3 + 0], 0, net_w) * scale_w
            ly = clamp(det[5 + p * 3 + 1], 0, net_h) * scale_h
            landmarks.append((int(lx), int(ly)))
        boxes.append({
            "box": [int(x1), int(y1), int(x2), int(y2)],
            "conf": float(conf),
            "landmarks": landmarks
        })
    # NMS
    boxes = sorted(boxes, key=lambda x: x["conf"], reverse=True)
    keep = []
    while boxes:
        curr = boxes.pop(0)
        keep.append(curr)
        boxes = [
            b for b in boxes
            if compute_iou(curr["box"], b["box"]) <= nms_thresh
        ]
    return keep

def compute_iou(box1, box2):
    x1 = max(box1[0], box2[0])
    y1 = max(box1[1], box2[1])
    x2 = min(box1[2], box2[2])
    y2 = min(box1[3], box2[3])
    inter_w = max(0, x2 - x1)
    inter_h = max(0, y2 - y1)
    inter = inter_w * inter_h
    area1 = (box1[2] - box1[0]) * (box1[3] - box1[1])
    area2 = (box2[2] - box2[0]) * (box2[3] - box2[1])
    union = area1 + area2 - inter
    return inter / union if union > 0 else 0

def draw_boxes(img, detections):
    for det in detections:
        x1, y1, x2, y2 = det["box"]
        cv2.rectangle(img, (x1, y1), (x2, y2), (0,255,0), 2)
        for (lx, ly) in det["landmarks"]:
            cv2.circle(img, (lx, ly), 2, (0,0,255), -1)
        cv2.putText(img, f"{det['conf']:.2f}", (x1, y1-5), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255,0,0), 1)
    return img

if __name__ == "__main__":
    start_total = time.time()
    image_path = sys.argv[1] if len(sys.argv) > 1 else default_image_path
    net_w, net_h = 640, 640
    input_shape = (1, 3, net_h, net_w)

    start_pre = time.time()
    img_input, orig_img = preprocess_image(image_path, input_shape)
    end_pre = time.time()

    # Triton Inference
    client = httpclient.InferenceServerClient(url="localhost:8000")
    inputs = [httpclient.InferInput("input", img_input.shape, "FP32")]
    inputs[0].set_data_from_numpy(img_input)
    outputs = [httpclient.InferRequestedOutput("output")]

    start_inf = time.time()
    response = client.infer("yoloface", inputs=inputs, outputs=outputs)
    result = response.as_numpy("output")
    end_inf = time.time()
    print("Output shape:", result.shape)

    # Postprocess and visualize
    start_post = time.time()
    detections = postprocess(result, orig_img, net_w, net_h)
    annotated = draw_boxes(orig_img.copy(), detections)
    end_post = time.time()
    out_path = os.path.splitext(image_path)[0] + "_annotated.jpg"
    cv2.imwrite(out_path, annotated)
    print(f"Annotated image saved to {out_path}")

    end_total = time.time()
    print(f"Preprocessing time: {end_pre - start_pre:.4f} s")
    print(f"Inference time: {end_inf - start_inf:.4f} s")
    print(f"Postprocessing time: {end_post - start_post:.4f} s")
    print(f"Total time: {end_total - start_total:.4f} s")