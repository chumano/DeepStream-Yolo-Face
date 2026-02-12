# Using Triton Inference Server with YOLO ONNX Model

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
pip install tritonclient[http]
# or for gRPC
pip install tritonclient[grpc]
# or for shared memory
pip install tritonclient[shared_memory]
# or for all protocols
pip install tritonclient[all]
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

- [Triton Inference Server Documentation](https://github.com/triton-inference-server/server)
- [Triton Python Client](https://github.com/triton-inference-server/client)
