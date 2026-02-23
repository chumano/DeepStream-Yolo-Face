# DeepStream with Triton Inference Server Support

## Overview

The DeepStream application now supports both standard nvinfer and Triton Inference Server (nvinferserver) backends for inference.

## Configuration Files

### Standard nvinfer Configuration
- `configs/config_infer_primary_yoloV8_face.txt` - Standard ONNX/TensorRT inference

### Triton nvinferserver Configuration
- `configs/config_infer_primary_yoloV8_face_triton.txt` - Triton inference configuration

## Usage

### Running with Standard nvinfer (Default)

```bash
python3 deepstream.py \
  -s file:///app/videos/faces_tracking.mp4 \
  -c /app/configs/config_infer_primary_yoloV8_face.txt
```

### Running with Triton Inference Server

```bash
python3 deepstream.py \
  -s file:///app/videos/faces_tracking.mp4 \
  -c /app/configs/config_infer_primary_yoloV8_face_triton.txt \
  --use-triton \
  --kafka-broker kafka:29092 \
  --kafka-topic face-detections
```

## Command-Line Arguments

- `-s, --source`: Source stream/file (required)
- `-c, --infer-config`: Configuration file path (required)
- `--use-triton`: Use Triton Inference Server (nvinferserver) instead of nvinfer
- `-b, --streammux-batch-size`: Streammux batch size (default: 1)
- `-w, --streammux-width`: Streammux width (default: 1920)
- `-e, --streammux-height`: Streammux height (default: 1080)
- `-g, --gpu-id`: GPU ID (default: 0)
- `--kafka-broker`: Kafka broker address
- `--kafka-topic`: Kafka topic name (default: face-detections)
- `--kafka-delay`: Delay before sending to Kafka (default: 2.0)
- `--kafka-quality-threshold`: Quality improvement threshold for resend (default: 0.005)


## Benefits of Using Triton

- **Model Versioning**: Support multiple model versions
- **Dynamic Batching**: Automatic batching for improved throughput
- **Model Ensemble**: Chain multiple models together
- **Multi-Framework**: Support for TensorFlow, PyTorch, ONNX, TensorRT, etc.
- **Scalability**: Better resource utilization and performance
- **Remote Inference**: Inference can be done on a separate server

## Troubleshooting

### nvinferserver Element Not Found

If you see an error about nvinferserver not being available:

1. Check DeepStream installation:
   ```bash
   gst-inspect-1.0 nvinferserver
   ```

2. Ensure you have DeepStream 6.0+ installed, which includes nvinferserver support

### Triton Connection Issues

If the application cannot connect to Triton:

1. Verify Triton server is running:
   ```bash
   curl -v triton:8000/v2/health/ready
   ```

2. Check Triton logs for errors

3. Ensure the model is loaded properly:
   ```bash
   curl triton:8000/v2/models/yoloface
   ```

## Configuration Details

### Standard nvinfer Configuration

The standard configuration uses ONNX or TensorRT engines directly:
- Model file: `onnx-file` and `model-engine-file`
- Custom parsing: `parse-bbox-instance-mask-func-name`
- TensorRT engine built locally

### Triton Configuration

The Triton configuration uses the Triton Inference Server:
- Model repository: Points to Triton model repository
- Backend: `triton`
- Model served by Triton server (local or remote)
- Configuration in `config.pbtxt` format

## Architecture

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐
│  Source     │────▶│  nvstreammux │────▶│ nvinfer /   │
│  (camera/   │     │              │     │ nvinferserver│
│   file)     │     └──────────────┘     └─────────────┘
└─────────────┘                                  │
                                                 ▼
                                          ┌─────────────┐
                                          │  nvtracker  │
                                          └─────────────┘
                                                 │
                                                 ▼
                                          ┌─────────────┐
                                          │   nvdsosd   │
                                          └─────────────┘
                                                 │
                                                 ▼
                                          ┌─────────────┐
                                          │   Display   │
                                          └─────────────┘
```

When using `--use-triton`, the `nvinferserver` element connects to a Triton Inference Server instance for model inference.

## Additional Resources

- [DeepStream Documentation](https://docs.nvidia.com/metropolis/deepstream/dev-guide/)
- [Triton Inference Server Documentation](https://docs.nvidia.com/deeplearning/triton-inference-server/)
- [nvinferserver Plugin Guide](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvinferserver.html)
