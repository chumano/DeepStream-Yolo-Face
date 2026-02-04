# TODO

## deepstream
### Python
- get timestamp from frame metadata
- save frame images with bounding boxes
  - save image to file
  - send metadata to Kafka (include image path)
- improve face assessment (blur, small faces, wear mask, ...)
- handle multi camera input

### C
- run c program

### C++
cp deepstream.c deepstream.cpp & make KAFKA=1

## Consumer applications
- storage to Qdrant
- storage images to disk
- streaming with mjpeg 