
import onnx
from onnx import checker

# Usage: python scripts/model_inspect.py

print("onnx version:", onnx.__version__)

m = onnx.load("./models/yolov8n-face.onnx")

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
checker.check_graph(m.graph)

# print opset version
print("ONNX model opset version:", m.opset_import[0].version)

print("\nONNX model is valid.")