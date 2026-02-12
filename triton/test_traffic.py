import numpy as np
import tritonclient.http as httpclient
# Usage: python triton/test_face.py

client = httpclient.InferenceServerClient(url="localhost:8000")
input_data = np.random.rand(1, 3, 640, 640).astype(np.float32)

# Update input name to "images"
inputs = [httpclient.InferInput("images", input_data.shape, "FP32")]
inputs[0].set_data_from_numpy(input_data)

# Update outputs to match model
outputs = [
    httpclient.InferRequestedOutput("num_dets"),
    httpclient.InferRequestedOutput("det_boxes"),
    httpclient.InferRequestedOutput("det_scores"),
    httpclient.InferRequestedOutput("det_classes"),
]

response = client.infer("traffic", inputs=inputs, outputs=outputs)

# Fetch and print output shapes
num_dets = response.as_numpy("num_dets")
det_boxes = response.as_numpy("det_boxes")
det_scores = response.as_numpy("det_scores")
det_classes = response.as_numpy("det_classes")

print("num_dets shape:", num_dets.shape)
print("det_boxes shape:", det_boxes.shape)
print("det_scores shape:", det_scores.shape)
print("det_classes shape:", det_classes.shape)