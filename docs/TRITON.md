# Using Triton Inference Server with YOLO ONNX Model

Container Version: `nvcr.io/nvidia/tritonserver:24.12-py3`
Triton Inference Server Version: 2.53

https://docs.nvidia.com/deeplearning/triton-inference-server/archives/
triton-inference-server-2500/user-guide/docs/user_guide/performance_tuning.html

## Prepare the Model Repository

Place your ONNX model and the `config.pbtxt` file in the following structure:
```
model-repo/
  yolo/
    1/
      model.onnx
    config.pbtxt
```


## Check server run

```bash
curl -v localhost:8000/v2/health/ready
```


## Sending Inference Requests
### Model Input/Output

- **Input:** `input` — shape `[None, 3, 640, 640]`, type `FP32`
- **Output:** `output` — shape `[None, 8400, 20]`, type `FP32`

### Example
Install the Triton client library if you haven't already:
```bash
wsl
source .venv/bin/activate

pip install tritonclient[http]==2.65.0
# or for gRPC
pip install tritonclient[grpc]==2.65.0
# or for all protocols
pip install tritonclient[all]==2.65.0
```

You can use the Triton Python client:
```python
import numpy as np
import tritonclient.http as httpclient

client = httpclient.InferenceServerClient(url="localhost:8000")
input_data = np.random.rand(1, 3, 640, 640).astype(np.float32)
inputs = [httpclient.InferInput("input", input_data.shape, "FP32")]
inputs[0].set_data_from_numpy(input_data)
outputs = [httpclient.InferRequestedOutput("output")]

response = client.infer("yolo", inputs=inputs, outputs=outputs)
result = response.as_numpy("output")
print(result.shape)
```

## References
- [Release Notes for Triton Inference Server](https://docs.nvidia.com/deeplearning/triton-inference-server/release-notes/rel-25-05.html)
- [Triton Server Container](https://catalog.ngc.nvidia.com/orgs/nvidia/containers/tritonserver/tags?version=25.12-py3)
- [Triton Inference Server Documentation](https://github.com/triton-inference-server/server)
- [Triton Python Client](https://github.com/triton-inference-server/client)
