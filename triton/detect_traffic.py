import numpy as np
import tritonclient.http as httpclient
from PIL import Image, ImageDraw, ImageFont
import sys

# Usage: python triton/detect_traffic.py [image_path]
default_image_path = "images/traffic.jpg"


def preprocess_image(image_path, input_shape):
    img = Image.open(image_path).convert("RGB")
    img_resized = img.resize((input_shape[3], input_shape[2]))
    img_np = np.array(img_resized).astype(np.float32) / 255.0
    img_np = np.transpose(img_np, (2, 0, 1))  # HWC to CHW
    img_np = np.expand_dims(img_np, axis=0)   # Add batch dim
    return img, img_np


def clip(val, min_val, max_val):
    return max(min(val, max_val), min_val)


def scale_box_to_image(box, network_width, network_height, orig_width, orig_height):
    left, top, right, bottom = box
    left = left * orig_width / network_width
    top = top * orig_height / network_height
    right = right * orig_width / network_width
    bottom = bottom * orig_height / network_height
    return [left, top, right, bottom]


def parse_outputs(num_dets, det_boxes, det_scores, det_classes, network_width, network_height, threshold=0.5, orig_width=None, orig_height=None):
    objects = []
    # Remove batch dimension if present
    if num_dets.ndim == 2:
        num_dets = num_dets[0]
    if det_boxes.ndim == 3:
        det_boxes = det_boxes[0]
    if det_scores.ndim == 2:
        det_scores = det_scores[0]
    if det_classes.ndim == 2:
        det_classes = det_classes[0]
    # Ensure keep_count is a scalar
    keep_count = int(num_dets.item()) if num_dets.size == 1 else int(num_dets[0].item())
    for i in range(keep_count):
        # Extract scalar values for cls and score
        cls = int(det_classes[i]) if np.isscalar(det_classes[i]) else int(det_classes[i].item())
        score = float(det_scores[i]) if np.isscalar(det_scores[i]) else float(det_scores[i].item())
        if score < threshold:
            continue
        x1 = float(det_boxes[i, 0])
        y1 = float(det_boxes[i, 1])
        x2 = float(det_boxes[i, 2])
        y2 = float(det_boxes[i, 3])
        if x2 < x1 or y2 < y1:
            continue
        left = clip(x1, 0, network_width - 1)
        top = clip(y1, 0, network_height - 1)
        right = clip(x2, 0, network_width - 1)
        bottom = clip(y2, 0, network_height - 1)
        width = right - left
        height = bottom - top
        if width < 0 or height < 0:
            continue
        box = [left, top, right, bottom]
        # Scale to original image size if provided
        if orig_width and orig_height:
            box = scale_box_to_image(box, network_width, network_height, orig_width, orig_height)
        objects.append({
            "classId": cls,
            "score": score,
            "box": box
        })
    return objects


def draw_annotations(img, objects, class_names=None):
    draw = ImageDraw.Draw(img)
    font = None
    try:
        font = ImageFont.truetype("arial.ttf", 16)
    except:
        font = ImageFont.load_default()
    for obj in objects:
        left, top, right, bottom = obj["box"]
        draw.rectangle([left, top, right, bottom], outline="red", width=2)
        label = f"{obj['classId']}:{obj['score']:.2f}"
        if class_names and obj['classId'] < len(class_names):
            label = f"{class_names[obj['classId']]}:{obj['score']:.2f}"
        draw.text((left, top), label, fill="red", font=font)
    return img


if __name__ == "__main__":
    image_path = sys.argv[1] if len(sys.argv) > 1 else default_image_path
    # Model expects (1, 3, 640, 640)
    input_shape = (1, 3, 640, 640)
    orig_img, input_data = preprocess_image(image_path, input_shape)
    orig_width, orig_height = orig_img.size

    client = httpclient.InferenceServerClient(url="localhost:8000")
    inputs = [httpclient.InferInput("images", input_data.shape, "FP32")]
    inputs[0].set_data_from_numpy(input_data)

    outputs = [
        httpclient.InferRequestedOutput("num_dets"),
        httpclient.InferRequestedOutput("det_boxes"),
        httpclient.InferRequestedOutput("det_scores"),
        httpclient.InferRequestedOutput("det_classes"),
    ]

    response = client.infer("traffic", inputs=inputs, outputs=outputs)

    num_dets = response.as_numpy("num_dets")
    det_boxes = response.as_numpy("det_boxes")
    det_scores = response.as_numpy("det_scores")
    det_classes = response.as_numpy("det_classes")

    print("num_dets shape:", num_dets.shape)
    print("det_boxes shape:", det_boxes.shape)
    print("det_scores shape:", det_scores.shape)
    print("det_classes shape:", det_classes.shape)

    # Parse detections
    objects = parse_outputs(
        num_dets,
        det_boxes,
        det_scores,
        det_classes,
        network_width=input_shape[3],
        network_height=input_shape[2],
        threshold=0.5,
        orig_width=orig_width,
        orig_height=orig_height
    )

    # Draw and save output
    annotated_img = draw_annotations(orig_img, objects)
    output_path = image_path.replace(".jpg", "_annotated.jpg")
    annotated_img.save(output_path)
    print(f"Saved annotated image to {output_path}")