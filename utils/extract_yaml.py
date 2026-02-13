import os
import onnx
import torch
import torch.nn as nn
from copy import deepcopy
from importlib import reload
import yaml

from ultralytics import YOLO

# Usage : python3 extract_yaml.py
pt_file = "yolov8n-face.pt"
nn_file = "yolov8n-face1.yaml"

# Load YOLO model from .pt file
model = YOLO(pt_file, task="detect")

# Extract model architecture (as a dict)
arch_dict = model.model.yaml

# Print architecture
print("Model architecture:")
print(arch_dict)

# Optionally, save to YAML file
yaml_path = nn_file
with open(yaml_path, "w") as f:
    yaml.dump(arch_dict, f)

print(f"Architecture saved to {yaml_path}")

# load back the yaml file to model
