import os
import types
import onnx
import torch
import torch.nn as nn

import models
from models.experimental import attempt_load
from utils.activations import Hardswish, SiLU


class DeepStreamOutput(nn.Module):
    def __init__(self):
        super().__init__()

    def forward(self, x):
        boxes = x[:, :, :4]
        convert_matrix = torch.tensor(
            [[1, 0, 1, 0], [0, 1, 0, 1], [-0.5, 0, 0.5, 0], [0, -0.5, 0, 0.5]], dtype=boxes.dtype, device=boxes.device
        )
        boxes @= convert_matrix
        objectness = x[:, :, 4:5]
        scores = x[:, :, 15:]
        scores *= objectness
        landmarks = x[:, :, 5:15]
        return torch.cat([boxes, scores, landmarks], dim=-1)


def forward_deepstream(self, x):
    z = []

    for i in range(self.nl):
        x[i] = self.m[i](x[i])
        bs, _, ny, nx = x[i].shape
        x[i] = x[i].view(bs, self.na, self.no, ny, nx).permute(0, 1, 3, 4, 2).contiguous()

        if self.grid[i].shape[2:4] != x[i].shape[2:4]:
            self.grid[i], self.anchor_grid[i] = self._make_grid_new(nx, ny,i)

        y = torch.full_like(x[i], 0)
        y = y + torch.cat([
            x[i][:, :, :, :, 0:5].sigmoid(),
            torch.cat([x[i][:, :, :, :, 5:15], x[i][:, :, :, :, 15:15+self.nc].sigmoid()], 4)
        ], 4)

        box_xy = (y[..., 0:2] * 2. - 0.5 + self.grid[i].to(x[i].device)) * self.stride[i]
        box_wh = (y[..., 2:4] * 2) ** 2 * self.anchor_grid[i]

        landm1 = x[i][..., 5:7] * self.anchor_grid[i] + self.grid[i].to(x[i].device) * self.stride[i]
        landm2 = x[i][..., 7:9] * self.anchor_grid[i] + self.grid[i].to(x[i].device) * self.stride[i]
        landm3 = x[i][..., 9:11] * self.anchor_grid[i] + self.grid[i].to(x[i].device) * self.stride[i]
        landm4 = x[i][..., 11:13] * self.anchor_grid[i] + self.grid[i].to(x[i].device) * self.stride[i]
        landm5 = x[i][..., 13:15] * self.anchor_grid[i] + self.grid[i].to(x[i].device) * self.stride[i]

        y = torch.cat([
            box_xy, box_wh, y[:, :, :, :, 4:5], landm1, landm2, landm3, landm4, landm5, y[:, :, :, :, 15:15+self.nc]
        ], -1)

        z.append(y.view(bs, -1, self.no))

    return torch.cat(z, 1)


def yolov5_face_export(weights, device, fuse=True):
    model = attempt_load(weights, map_location=device)
    delattr(model.model[-1], "anchor_grid")
    model.model[-1].anchor_grid = [torch.zeros(1)] * 3
    model.eval()
    for k, m in model.named_modules():
        m._non_persistent_buffers_set = set()
        if isinstance(m, models.common.Conv):
            if isinstance(m.act, nn.Hardswish):
                m.act = Hardswish()
            elif isinstance(m.act, nn.SiLU):
                m.act = SiLU()
        if isinstance(m, models.common.ShuffleV2Block):
            for i in range(len(m.branch1)):
                if isinstance(m.branch1[i], nn.SiLU):
                    m.branch1[i] = SiLU()
            for i in range(len(m.branch2)):
                if isinstance(m.branch2[i], nn.SiLU):
                    m.branch2[i] = SiLU()
    model.model[-1].forward = types.MethodType(forward_deepstream, model.model[-1])
    if fuse:
        model.fuse()
    return model


def suppress_warnings():
    import warnings
    warnings.filterwarnings("ignore", category=torch.jit.TracerWarning)
    warnings.filterwarnings("ignore", category=UserWarning)
    warnings.filterwarnings("ignore", category=DeprecationWarning)
    warnings.filterwarnings("ignore", category=FutureWarning)
    warnings.filterwarnings("ignore", category=ResourceWarning)


def main(args):
    suppress_warnings()

    print(f"\nStarting: {args.weights}")

    print("Opening YOLOv5-Face model")

    device = torch.device("cpu")
    model = yolov5_face_export(args.weights, device)

    if hasattr(model, "names") and len(model.names) > 0:
        print("Creating labels.txt file")
        with open("labels.txt", "w", encoding="utf-8") as f:
            for name in model.names:
                f.write(f"{name}\n")

    model = nn.Sequential(model, DeepStreamOutput())

    img_size = args.size * 2 if len(args.size) == 1 else args.size

    if img_size == [640, 640] and args.p6:
        img_size = [1280] * 2

    onnx_input_im = torch.zeros(args.batch, 3, *img_size).to(device)
    onnx_output_file = args.weights.rsplit(".", 1)[0] + ".onnx"

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
        import onnxslim
        model_onnx = onnx.load(onnx_output_file)
        model_onnx = onnxslim.slim(model_onnx)
        onnx.save(model_onnx, onnx_output_file)

    print(f"Done: {onnx_output_file}\n")


def parse_args():
    import argparse
    parser = argparse.ArgumentParser(description="DeepStream YOLOv5-Face conversion")
    parser.add_argument("-w", "--weights", required=True, type=str, help="Input weights (.pt) file path (required)")
    parser.add_argument("-s", "--size", nargs="+", type=int, default=[640], help="Inference size [H,W] (default [640])")
    parser.add_argument("--p6", action="store_true", help="P6 model")
    parser.add_argument("--opset", type=int, default=17, help="ONNX opset version")
    parser.add_argument("--simplify", action="store_true", help="ONNX simplify model")
    parser.add_argument("--dynamic", action="store_true", help="Dynamic batch-size")
    parser.add_argument("--batch", type=int, default=1, help="Static batch-size")
    args = parser.parse_args()
    if not os.path.isfile(args.weights):
        raise SystemExit("Invalid weights file")
    if args.dynamic and args.batch > 1:
        raise SystemExit("Cannot set dynamic batch-size and static batch-size at same time")
    return args


if __name__ == "__main__":
    args = parse_args()
    main(args)
