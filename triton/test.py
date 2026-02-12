import numpy as np
import tritonclient.http as httpclient
# Usage: python triton/test.py

client = httpclient.InferenceServerClient(url="localhost:8000")
input_data = np.random.rand(1, 3, 640, 640).astype(np.float32)
inputs = [httpclient.InferInput("input", input_data.shape, "FP32")]
inputs[0].set_data_from_numpy(input_data)
outputs = [httpclient.InferRequestedOutput("output")]

response = client.infer("yoloface", inputs=inputs, outputs=outputs)
result = response.as_numpy("output")
print(result.shape)