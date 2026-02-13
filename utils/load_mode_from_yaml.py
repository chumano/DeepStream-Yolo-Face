import os
import torch
import torch.nn as nn
from copy import deepcopy
from importlib import reload
import yaml

from ultralytics import YOLO
from ultralytics.nn.tasks import attempt_load_one_weight, parse_model
from ultralytics.yolo.utils import DEFAULT_CFG

# Usage : python3 load_model_from_yaml.py -y yolov8n-face1.yaml

def suppress_warnings():
    import warnings
    warnings.filterwarnings("ignore", category=torch.jit.TracerWarning)
    warnings.filterwarnings("ignore", category=UserWarning)
    warnings.filterwarnings("ignore", category=DeprecationWarning)
    warnings.filterwarnings("ignore", category=FutureWarning)
    warnings.filterwarnings("ignore", category=ResourceWarning)

def print_output_structure(output, prefix=""):
    if isinstance(output, (tuple, list)):
        for i, out in enumerate(output):
            print_output_structure(out, prefix + f"[{i}]")
    else:
        print(f"  {prefix} shape: {getattr(output, 'shape', type(output))}")

class DeepStreamOutput(nn.Module):
    def __init__(self):
        super().__init__()

    def forward(self, x):
        y = x[0].transpose(1, 2)
        return y


#  model,cfg =load_yolo_from_yaml('yolov8n-face.yaml',device)
def load_yolo_from_yaml(yaml_path, device):
    with open(yaml_path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    # Parse model
    #model, _ = parse_model(deepcopy(cfg), ch=cfg.get("ch", 3), verbose=True)

    #  YOLO(model='yolov8n-face.yaml', task="detect")
    model = YOLO(model=yaml_path, task="detect")
    model.to(device)
    return model, cfg

def main(args):
    suppress_warnings()
    print(f"\nStarting: {args.yaml}")

    device = torch.device("cpu")
    model, cfg = load_yolo_from_yaml(args.yaml, device)


    # Save labels.txt if class names are present
    if "names" in cfg and isinstance(cfg["names"], dict) and len(cfg["names"].keys()) > 0:
        print("Creating labels.txt file")
        with open("labels.txt", "w", encoding="utf-8") as f:
            for name in cfg["names"].values():
                f.write(f"{name}\n")

    img_size = args.size * 2 if len(args.size) == 1 else args.size
    onnx_input_im = torch.zeros(args.batch, cfg.get("ch", 3), *img_size).to(device)
    onnx_output_file = args.yaml.rsplit(".", 1)[0] + ".onnx"
    print("input shape:", onnx_input_im.shape)

    model = nn.Sequential(model.model, DeepStreamOutput())


    dynamic_axes = {
        "input": {
            0: "batch"
        },
        "output": {
            0: "batch"
        }
    }

    print("Exporting the model to ONNX")
    torch.onnx.export(
        model,
        onnx_input_im,
        onnx_output_file,
        verbose=False,
        opset_version=args.opset,
        do_constant_folding=True,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes=dynamic_axes if args.dynamic else None
    )

    if args.simplify:
        print("Simplifying the ONNX model")
        import onnx
        import onnxslim
        model_onnx = onnx.load(onnx_output_file)
        model_onnx = onnxslim.slim(model_onnx)
        onnx.save(model_onnx, onnx_output_file)

    print(f"Done: {onnx_output_file}\n")

def parse_args():
    import argparse
    parser = argparse.ArgumentParser(description="DeepStream YOLOv8-Face conversion from YAML")
    parser.add_argument("-y", "--yaml", required=True, type=str, help="Input model config (.yaml) file path (required)")
    parser.add_argument("-s", "--size", nargs="+", type=int, default=[640], help="Inference size [H,W] (default [640])")
    parser.add_argument("--opset", type=int, default=17, help="ONNX opset version")
    parser.add_argument("--simplify", action="store_true", help="ONNX simplify model")
    parser.add_argument("--dynamic", action="store_true", help="Dynamic batch-size")
    parser.add_argument("--batch", type=int, default=1, help="Static batch-size")
    args = parser.parse_args()
    if not os.path.isfile(args.yaml):
        raise SystemExit("Invalid yaml file")
    if args.dynamic and args.batch > 1:
        raise SystemExit("Cannot set dynamic batch-size and static batch-size at same time")
    return args

if __name__ == "__main__":
    args = parse_args()
    main(args)
