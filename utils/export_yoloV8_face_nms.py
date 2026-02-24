import os
import ultralytics
from ultralytics import YOLO
from ultralytics.yolo.utils import LOGGER
import torch

# NOTE: This script is NOT working. NEED TO FIX

print("PyTorch version:", torch.__version__)
# Check cuda availability
print("CUDA available:", torch.cuda.is_available())
print("CUDA device count:", torch.cuda.device_count())
# set logging level to DEBUG
LOGGER.setLevel("DEBUG")

# print ultralytics version
print("Ultralytics version:", ultralytics.__version__)

# cd tmp/yolov8-face
# Usage : python3 export_yoloV8_face_nms.py
model = YOLO("yolov8n-face.pt",)

# Display model information (optional)
print("\nModel Information:")
model.info()

print("\nExporting model with NMS...")
try:
    export_result = model.export(
        format="onnx", dynamic=True, opset=17,
        simplify=True, device="cpu",
        half=False,  # use FP32
        nms=True,
        conf=0.25,
        iou=0.45
    )
except Exception as e:
    print(f"Model export failed: {e}")
    exit(1)

onnx_file = "yolov8n-face.onnx"
onnx_nms_file = "yolov8n-face-nms.onnx"
print("\nRenaming exported model file...")

# Check export result and file existence before renaming
if export_result is None or not os.path.exists(onnx_file):
    print(f"Export failed or ONNX file not found: {onnx_file}")
else:
    try:
        os.rename(onnx_file, onnx_nms_file)
        print(f"Renamed {onnx_file} to {onnx_nms_file}")
    except Exception as e:
        print(f"Failed to rename file: {e}")