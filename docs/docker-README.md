# Docker Setup for DeepStream-Yolo-Face
https://docs.nvidia.com/metropolis/deepstream/7.1/python-api/PYTHON_API/NvOSD/NvOSD_toc.html

## RUN
REMEMBER: in run docker in WSL2

`docker compose exec -it deepstream-yolo-face-dev /bin/bash`

Then inside container run inference:
```bash
GST_DEBUG=*:3 python3 deepstream.py \
    -s file:///app/videos/friends_short.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt

python3 deepstream.py \
    -s file:///app/videos/jefferson_fisher.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt
python3 deepstream.py \
    -s file:///app/videos/faces_tracking.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt

# send to kafka `pip3 install kafka-python==2.3.0`
python3 deepstream.py \
  -s file:///app/videos/faces_tracking.mp4 \
  -c /app/configs/config_infer_primary_yoloV8_face.txt \
  --kafka-broker kafka:29092 \
  --kafka-topic face-detections

python3 deepstream.py -s "rtsp://admin:dt%4012345@192.168.1.203:554/cam/realmonitor?channel=1&subtype=0&unicast=true&proto=Onvif" -c /app/configs/config_infer_primary_yoloV8_face.txt --kafka-broker kafka:29092 --kafka-topic face-detections
##################
# or C app
rm outputs/frames/*

./deepstream \
    -s file:///app/videos/friends_short.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt

./deepstream \
    -s file:///app/videos/faces_tracking.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt
# send to kafka
./deepstream \
  -s file:///app/videos/faces_tracking.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt \
    --kafka-broker kafka:29092 \
    --kafka-topic face-detections

# multi source
./deepstream \
  -s file:///app/videos/faces_tracking.mp4 \
  -s file:///app/videos/friends_short.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt \
    --kafka-broker kafka:29092 \
    --kafka-topic face-detections --disable-display 

./deepstream \
  -s file:///app/videos/friends.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt \
    --kafka-broker kafka:29092 \
    --kafka-topic face-detections --disable-display --disable-crop-image

GST LOG LEVEL
https://gstreamer.freedesktop.org/documentation/tutorials/basic/debugging-tools.html?gi-language=c

```
 # | Name    | Description                                                    |
|---|---------|----------------------------------------------------------------|
| 0 | none    | No debug information is output.                                |
| 1 | ERROR   | Logs all fatal errors. These are errors that do not allow the  |
|   |         | core or elements to perform the requested action. The          |
|   |         | application can still recover if programmed to handle the      |
|   |         | conditions that triggered the error.                           |
| 2 | WARNING | Logs all warnings. Typically these are non-fatal, but          |
|   |         | user-visible problems are expected to happen.                  |
| 3 | FIXME   | Logs all "fixme" messages. Those typically that a codepath that|
|   |         | is known to be incomplete has been triggered. It may work in   |
|   |         | most cases, but may cause problems in specific instances.      |
| 4 | INFO    | Logs all informational messages. These are typically used for  |
|   |         | events in the system that only happen once, or are important   |
|   |         | and rare enough to be logged at this level.                    |
| 5 | DEBUG   | Logs all debug messages. These are general debug messages for  |
|   |         | events that happen only a limited number of times during an    |
|   |         | object's lifetime; these include setup, teardown, change of    |
|   |         | parameters, etc.                                               |
| 6 | LOG     | Logs all log messages. These are messages for events that      |
|   |         | happen repeatedly during an object's lifetime; these include   |
|   |         | streaming and steady-state conditions. This is used for log    |
|   |         | messages that happen on every buffer in an element for example.|
| 7 | TRACE   | Logs all trace messages. Those are message that happen very    |
|   |         | very often. This is for example is each time the reference     |
|   |         | count of a GstMiniObject, such as a GstBuffer or GstEvent, is  |
|   |         | modified.                                                      |
| 9 | MEMDUMP | Logs all memory dump messages. This is the heaviest logging and|
|   |         | may include dumping the content of blocks of memory.           |

## VERSIONS
https://github.com/NVIDIA-AI-IOT/deepstream_python_apps/releases
```bash
DEEPSTREAM_VERSION=8.0 PYDS_VERSION=1.2.2 CUDA_VER 12.8 PYTHON_VERSION=3.12 Ubuntu 24.04 GStreamer 1.24.2 TensorRT 10.9.0.34 NVIDIA driver 570.133.20
DEEPSTREAM_VERSION=7.1 PYDS_VERSION=1.2.0 CUDA_VER 12.6 PYTHON_VERSION=3.10 Ubuntu 22.04 GStreamer 1.20.3 TensorRT 10.3.0.26 NVIDIA driver 535.183.06 (for Data Center GPUs) and 560.35.03 (for RTX GPUs)
DEEPSTREAM_VERSION=6.4 PYDS_VERSION=1.1.10 CUDA_VER=12.2 PYTHON_VERSION=3.10 Ubuntu 22.04 GStreamer 1.20.3 TensorRT 8.6.1.6 NVIDIA driver 535.104.12
```
https://docs.nvidia.com/metropolis/deepstream/8.0/text/DS_Installation.html#prerequisites
https://docs.nvidia.com/metropolis/deepstream/7.1/text/DS_Installation.html#id1
https://docs.nvidia.com/metropolis/deepstream/6.4/dev-guide/text/DS_Quickstart.html#dgpu-setup-for-ubuntu

My NVIDIA driver version: 577.00 (RTX 3060) works with DeepStream 7.1 and 6.4

Check versions
```bash
# deepstream
dpkg -l | grep deepstream
cat /opt/nvidia/deepstream/deepstream/version

# pyds
pip3 list | grep pyds
python3 -c "import pyds; print(pyds.__version__)"

# Cuda
nvcc --version
nvidia-smi

# TensorRT
dpkg -l | grep tensorrt
dpkg -l | grep nvinfer
ls /usr/lib/x86_64-linux-gnu/libnvinfer.so*

# GStreamer
gst-launch-1.0 --version

# Python
python3 --version

# Ubuntu
lsb_release -a
cat /etc/os-release
```



## DeepStream base image:
```bash
docker pull nvcr.io/nvidia/deepstream:8.0-gc-triton-devel
docker save nvcr.io/nvidia/deepstream:8.0-gc-triton-devel -o ./temp/deepstream:8.0-gc-triton-devel.tar

docker load -i ./temp/deepstream:8.0-gc-triton-devel.tar
```

## Test deepstream
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
    -c /app/configs/config_infer_primary_yoloV8_face.txt

# Run with RTSP stream
docker-compose run --rm deepstream-yolo-face \
    python3 deepstream.py \
    -s rtsp://your-rtsp-url \
    -c /app/configs/config_infer_primary_yoloV8_face.txt

# Run C application
docker-compose run --rm deepstream-yolo-face \
    ./deepstream \
    -s file:///app/videos/friends_short.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt
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
    -c /app/configs/config_infer_primary_yoloV8_face.txt
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
    -v $(pwd)/outputs:/app/outputs:rw \
    -v $(pwd)/configs:/app/configs:ro \
    --network host \
    --ipc host \
    --shm-size=2g \
    deepstream-yolo-face:latest \
    python3 deepstream.py \
    -s file:///app/videos/your_video.mp4 \
    -c /app/configs/config_infer_primary_yoloV8_face.txt
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
