# TODO

## deepstream
### Python
- [x] get timestamp from frame metadata
- [x] save frame images with bounding boxes
  - [x] save image to file
  - [x] send metadata to Kafka (include image path)
- [x] support triton inference server

### C
- [x] get timestamp from frame metadata
- [x] save frame images with bounding boxes
  - [x] save image to file
  - [x] send metadata to Kafka (include image path)
- [x] handle multi camera inputs
- [x] config from key value file
- [x] support triton inference server
- [] improve save image frame using nvds object encoder - nvds_obj_enc_process. NOT WORKING ON WSL
  - https://forums.developer.nvidia.com/t/deepstream-8-0-docker-on-wsl2-nvds-obj-enc-process-returns-error-code-1-when-saving-images-segmentation-fault/346751/4
  - https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_Release_notes.html#limitations

### C++
- [x] refactor deepstream.c to deepstream.cpp
- [x] pipepline monitoring and metrics
- [x] pipeline elements configuration
- [x] handle frame with padding by nvstreammux 
  - frame image save exclude letterbox area (configurable)
  - bounding box need to be adjusted to ensure the correct position on the padded frame image
- [x] change uridecodebin to nvurisrcbin to support reconnition on RTSP stream with low latency
   - https://docs.nvidia.com/metropolis/deepstream/7.1/text/DS_plugin_gst-nvurisrcbin.html
```bash
gst-launch-1.0 nvurisrcbin \
uri=rtsp://100.64.0.153:8554/test! \
m.sink_0 nvstreammux name=m width=1280 height=720 batch-size=1 ! nvmultistreamtiler ! nveglglessink
```
  
- [x] support save orginal frame image get directly from source (before the nvstreammux) (configurable)
  - save when face detected (smart saving) or save all frames
  - buffered orginal frame image for a short period of time (e.g. 1 second)  and save the buffered image when face detected
  - support multi source inputs and save original frame image for each source
- [x] feature smart recording with nvurisrcbin. trigger recording by face detection and save video clip/
  - https://docs.nvidia.com/metropolis/deepstream/7.1/text/DS_Smart_video.html

- [x] smart-record sr-done callback to get the recorded video file path
   - [x] send to Kafka
- [x] clear old images/videos on thread
- [x] monitor drop frame in nvurisrcbin/rtspsrc

- [ ] frame_buffer push raw frame data, then encode jpeg in buffer worker thread when need to save (currently push encoded jpeg data to frame_buffer)


### Advanced features
- [ ] handle dynamic input sources (add/remove camera at runtime) through rest server
  - https://docs.nvidia.com/metropolis/deepstream/7.1/text/DS_plugin_gst-nvmultiurisrcbin.html

- [ ] improve face assessment (blur, small faces, wear mask, ...)

## Consumer applications
- [x] crop face images from frames if face_image not provided by deepstream
- [x] storage face images to disk
- [x] storage face embeddings to Qdrant
- [x] add annotation box to frame images and save to disk
- [ ] check region of interest (ROI) for face detection
- [ ] streaming with mjpeg 
- [ ] consumer with rule engine

## Video inspect (scripts/video) with OpenCV
 - [x] Get metadata from video file and print to console
 - [x] Convert video file to frame images and save to disk
 - [x] Convert frame images to video file and save to disk

## Model conversion
- [ ] add NMS to onnx model https://github.com/triple-mu/YOLOv8-TensorRT/blob/main/models/common.py#L27
- https://github.com/triple-mu/YOLOv8-TensorRT/blob/main/docs/Pose.md
- https://github.com/mimiliaogo/holohub/blob/a59e41657288604d9c0c6179e8b65cf31c6b85f3/applications/yolo_model_deployment/CMakeLists.txt
- https://stephencowchau.medium.com/stitching-non-max-suppression-nms-to-yolov8n-on-exported-onnx-model-1c625021b22

## RTSP
- SEI Injection:  SEI (Supplemental Enhancement Information)