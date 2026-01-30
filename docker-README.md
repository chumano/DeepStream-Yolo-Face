# Docker Setup for DeepStream-Yolo-Face

DeepStream base image:
```bash
docker pull nvcr.io/nvidia/deepstream:8.0-gc-triton-devel
docker save nvcr.io/nvidia/deepstream:8.0-gc-triton-devel -o ./temp/deepstream:8.0-gc-triton-devel.tar

docker load -i ./temp/deepstream:8.0-gc-triton-devel.tar
```

Text deepstream
```bash
cd /opt/nvidia/deepstream/deepstream/samples/streams

gst-launch-1.0 filesrc location= sample_720p.mp4 ! qtdemux ! h264parse ! nvv4l2decoder ! nveglglessink -v

# run with nvstreammux
gst-launch-1.0 \
  videotestsrc ! \
  nvvideoconvert ! \
  'video/x-raw(memory:NVMM), format=NV12' ! \
  mux.sink_0 \
  nvstreammux name=mux batch-size=1 width=300 height=200 ! \
  nvvideoconvert ! \
  nvdsosd ! \
  nveglglessink

GST_DEBUG=*:3 python3 deepstream.py \
    -s file:///app/videos/friends_short.mp4 \
    -c config_infer_primary_yoloV8_face.txt
```

## Prerequisites

1. **NVIDIA Docker Runtime**: Install nvidia-docker2
   ```bash
   # Ubuntu/Debian
   distribution=$(. /etc/os-release;echo $ID$VERSION_ID)
   curl -s -L https://nvidia.github.io/nvidia-docker/gpgkey | sudo apt-key add -
   curl -s -L https://nvidia.github.io/nvidia-docker/$distribution/nvidia-docker.list | sudo tee /etc/apt/sources.list.d/nvidia-docker.list
   sudo apt-get update
   sudo apt-get install -y nvidia-docker2
   sudo systemctl restart docker
   ```

2. **X11 Display** (for GUI output):
   ```bash
   xhost +local:docker
   ```

## Directory Structure

Create these directories before running:
```bash
mkdir -p models videos outputs
```

- `models/`: Place your ONNX model files here
- `videos/`: Place your input video files here  
- `outputs/`: Output files will be saved here

## Building the Image

```bash
# Build with default DeepStream 8.0
docker-compose build

# Build with specific DeepStream version
docker-compose build --build-arg DEEPSTREAM_VERSION=7.1
```

## Running the Application

### Using docker-compose

```bash
# Run with default sample video
docker-compose up

# Run with custom video file
docker-compose run --rm deepstream-yolo-face \
    python3 deepstream.py \
    -s file:///app/videos/friends_short.mp4 \
    -c config_infer_primary_yoloV8_face.txt

# Run with RTSP stream
docker-compose run --rm deepstream-yolo-face \
    python3 deepstream.py \
    -s rtsp://your-rtsp-url \
    -c config_infer_primary_yoloV8_face.txt

# Run C application
docker-compose run --rm deepstream-yolo-face \
    ./deepstream \
    -s file:///app/videos/friends_short.mp4 \
    -c config_infer_primary_yoloV8_face.txt
```

### Development Mode (Shell Access)

```bash
# Start interactive shell
docker-compose --profile dev run --rm deepstream-yolo-face-dev

# Inside container, you can:
# - Export ONNX models
# - Build/rebuild libraries
# - Run inference

GST_DEBUG=*:3 python3 deepstream.py \
    -s file:///app/videos/friends_short.mp4 \
    -c config_infer_primary_yoloV8_face.txt
```

### Using docker run directly

```bash
# Run the container
docker run --rm -it \
    --runtime=nvidia \
    --gpus all \
    -e DISPLAY=$DISPLAY \
    -e NVIDIA_VISIBLE_DEVICES=all \
    -v /tmp/.X11-unix:/tmp/.X11-unix:rw \
    -v $(pwd)/models:/app/models:rw \
    -v $(pwd)/videos:/app/videos:ro \
    --network host \
    --ipc host \
    --shm-size=2g \
    deepstream-yolo-face:latest \
    python3 deepstream.py \
    -s file:///app/videos/your_video.mp4 \
    -c config_infer_primary_yoloV8_face.txt
```

## Model Setup

Before running inference, you need to:

1. **Download/Export ONNX Model**: Place your ONNX model in the `models/` directory

2. **Update config file**: Modify the config file to point to your model:
   ```ini
   [property]
   model-file=/app/models/your_model.onnx
   ```

3. **First Run**: The TensorRT engine will be generated on first run (may take 10+ minutes)

## Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `-s, --source` | Input source (file://, rtsp://, http://) | Required |
| `-c, --infer-config` | Inference config file | Required |
| `-b, --streammux-batch-size` | Batch size | 1 |
| `-w, --streammux-width` | Stream width | 1920 |
| `-e, --streammux-height` | Stream height | 1080 |
| `-g, --gpu-id` | GPU device ID | 0 |

## Troubleshooting

### Display Issues
```bash
# Allow X11 connections from Docker
xhost +local:docker

# Check DISPLAY variable
echo $DISPLAY
```

### GPU Not Found
```bash
# Verify NVIDIA runtime
docker info | grep -i nvidia

# Test GPU access
docker run --rm --gpus all nvidia/cuda:12.0-base nvidia-smi
```

### Permission Denied
```bash
# Add user to docker group
sudo usermod -aG docker $USER
newgrp docker
```

### Shared Memory Issues
```bash
# Increase shared memory size
docker-compose run --rm --shm-size=4g deepstream-yolo-face ...
```

## DeepStream Version Matrix

| DeepStream | CUDA (x86) | CUDA (Jetson) |
|------------|------------|---------------|
| 8.0 | 12.8 | 13.0 |
| 7.1 | 12.6 | 12.6 |
| 7.0 / 6.4 | 12.2 | 12.2 |
| 6.3 | 12.1 | 11.4 |
| 6.2 | 11.8 | 11.4 |

To use a different version, update the `DEEPSTREAM_VERSION` and `CUDA_VER` build args.
