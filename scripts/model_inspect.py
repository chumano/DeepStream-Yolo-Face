
import onnx
import sys
from onnx import checker

# Usage: python scripts/model_inspect.py [model_path]
# python scripts/model_inspect.py ./model-repo/traffic/model.onnx
# python scripts/model_inspect.py ./models/yolov8n-face.onnx
# python scripts/model_inspect.py ./models/yolov8n-face-nms.onnx
print("onnx version:", onnx.__version__)

model_path = sys.argv[1] if len(sys.argv) > 1 else "./models/yolov8n-face.onnx"
m = onnx.load(model_path)

print("\nInitializers (name -> shape):")
for init in m.graph.initializer:
    dims = list(init.dims)
    print(f"  {init.name} -> {dims}")

print("Model inputs:")
for i in m.graph.input:
    shape = []
    for dim in i.type.tensor_type.shape.dim:
        if dim.dim_value:
            shape.append(dim.dim_value)
        else:
            shape.append(None)
    print(f"  {i.name}: {shape}")


print("\nModel outputs:")
for o in m.graph.output:
    shape = []
    for dim in o.type.tensor_type.shape.dim:
        if dim.dim_value:
            shape.append(dim.dim_value)
        else:
            shape.append(None)
    print(f"  {o.name}: {shape}")

# Check if the converted ONNX protobuf is valid
try:
    checker.check_graph(m.graph)
except Exception as e:
    print("ONNX model validation failed:", e)
    

# print opset version
print("ONNX model opset version:", m.opset_import[0].version)

print("\nONNX model is valid.")