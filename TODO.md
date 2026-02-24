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
- [ ] handle dynamic input sources (add/remove camera at runtime)
- [ ] pipeline elements configuration
- [ ] pipepline monitoring and metrics
- [ ] check region of interest (ROI) for face detection
- [ ] improve face assessment (blur, small faces, wear mask, ...)

## Consumer applications
- [x] crop face images from frames if face_image not provided by deepstream
- [x] storage face images to disk
- [x] storage face embeddings to Qdrant
- [ ] streaming with mjpeg 
- [ ] consumer with rule engine

## Model conversion
- [ ] add NMS to onnx model https://github.com/triple-mu/YOLOv8-TensorRT/blob/main/models/common.py#L27
- https://github.com/triple-mu/YOLOv8-TensorRT/blob/main/docs/Pose.md
- https://github.com/mimiliaogo/holohub/blob/a59e41657288604d9c0c6179e8b65cf31c6b85f3/applications/yolo_model_deployment/CMakeLists.txt
- https://stephencowchau.medium.com/stitching-non-max-suppression-nms-to-yolov8n-on-exported-onnx-model-1c625021b22