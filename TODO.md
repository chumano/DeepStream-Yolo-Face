# TODO

## deepstream
### Python
- get timestamp from frame metadata
- save frame images with bounding boxes
  - save image to file
  - send metadata to Kafka (include image path)
- improve face assessment (blur, small faces, wear mask, ...)
- handle multi camera inputs

### C
- [x] get timestamp from frame metadata
- [x] save frame images with bounding boxes
  - [x] save image to file
  - [x] send metadata to Kafka (include image path)
- [x] handle multi camera inputs
- [x] config from key value file
- [] improve save image frame using nvds object encoder
- [] support triton inference server
- pipeline elements configuration
- pipepline monitoring and metrics
- check region of interest (ROI) for face detection
- handle dynamic input sources (add/remove camera at runtime)
- improve face assessment (blur, small faces, wear mask, ...)

## Consumer applications
- [x] crop face images from frames if face_image not provided by deepstream
- [x] storage face images to disk
- [ ] storage face embeddings to Qdrant
- [ ] streaming with mjpeg 
- [ ] consumer with rule engine

## Model conversion
- [ ] add NMS to onnx model https://github.com/triple-mu/YOLOv8-TensorRT/blob/main/models/common.py#L27
- https://github.com/triple-mu/YOLOv8-TensorRT/blob/main/docs/Pose.md
- https://github.com/mimiliaogo/holohub/blob/a59e41657288604d9c0c6179e8b65cf31c6b85f3/applications/yolo_model_deployment/CMakeLists.txt
- https://stephencowchau.medium.com/stitching-non-max-suppression-nms-to-yolov8n-on-exported-onnx-model-1c625021b22