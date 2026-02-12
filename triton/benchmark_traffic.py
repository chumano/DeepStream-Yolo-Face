import numpy as np
import tritonclient.http as httpclient
import time
# Usage: python triton/bendmark_traffic.py

client = httpclient.InferenceServerClient(url="localhost:8000")
input_data = np.random.rand(1, 3, 640, 640).astype(np.float32)

inputs = [httpclient.InferInput("images", input_data.shape, "FP32")]
inputs[0].set_data_from_numpy(input_data)

outputs = [
    httpclient.InferRequestedOutput("num_dets"),
    httpclient.InferRequestedOutput("det_boxes"),
    httpclient.InferRequestedOutput("det_scores"),
    httpclient.InferRequestedOutput("det_classes"),
]

num_runs = 1000
latencies = []
success_count = 0
error_count = 0

# Warmup
for _ in range(5):
    client.infer("traffic", inputs=inputs, outputs=outputs)

start = time.time()
for i in range(num_runs):
    t0 = time.time()
    try:
        response = client.infer("traffic", inputs=inputs, outputs=outputs)
        success_count += 1
        if i == 0:
            print("First inference response:")
            print(response)
    except Exception as e:
        error_count += 1
        if i == 0:
            print("First inference error:")
            print(e)
    t1 = time.time()
    latencies.append(t1 - t0)
end = time.time()

avg_latency = sum(latencies) / num_runs
throughput = num_runs / (end - start)

print(f"Benchmark results over {num_runs} runs:")
print(f"Average latency per inference: {avg_latency * 1000:.2f} ms")
print(f"Throughput: {throughput:.2f} inferences/sec")
print(f"Successful inferences: {success_count}")
print(f"Failed inferences: {error_count}")
