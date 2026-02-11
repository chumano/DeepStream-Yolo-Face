# Kafka Integration Guide

## SSH Tunnel (if accessing remote server)
```bash
ssh -L 9092:localhost:9092 dev3
```

## Prerequisites
```bash
pip install kafka-python
```

## Consume Messages from Docker Kafka Container

### Using Kafka Console Consumer (inside container)

1. **View all topics:**
```bash
docker exec -it ds-kafka kafka-topics --bootstrap-server localhost:9092 --list
```

2. **Consume from beginning:**
```bash
docker exec -it ds-kafka kafka-console-consumer \
  --bootstrap-server localhost:9092 \
  --topic face-detections \
  --from-beginning
```

3. **Consume live messages:**
```bash
docker exec -it ds-kafka kafka-console-consumer \
  --bootstrap-server localhost:9092 \
  --topic face-detections
```

4. **Consume with formatting:**
```bash
docker exec -it ds-kafka kafka-console-consumer \
  --bootstrap-server localhost:9092 \
  --topic face-detections \
  --from-beginning \
  --formatter kafka.tools.DefaultMessageFormatter \
  --property print.timestamp=true \
  --property print.key=true \
  --property print.value=true
```


## Topic Management

### Create topic manually (optional - auto-created by default):
```bash
docker exec -it kafka kafka-topics \
  --bootstrap-server localhost:9092 \
  --create \
  --topic face-detections \
  --partitions 3 \
  --replication-factor 1
```

### Describe topic:
```bash
docker exec -it kafka kafka-topics \
  --bootstrap-server localhost:9092 \
  --describe \
  --topic face-detections
```

### Delete topic:
```bash
docker exec -it kafka kafka-topics \
  --bootstrap-server localhost:9092 \
  --delete \
  --topic face-detections
```

## Message Format

Each detection message contains:
```json
{
  "timestamp": 1706659200.123,
  "object_id": 42,
  "source_id": 0,
  "class_id": 0,
  "confidence": 0.95,
  "frame_size": { "width": 1920, "height": 1080 },
  "bbox": {
    "left": 100.5,
    "top": 200.3,
    "width": 150.2,
    "height": 180.7
  },
  "crop_bbox": {
    "left": 90,
    "top": 180,
    "width": 170,
    "height": 220
  },
  "landmarks": [
    { "x": 120.1, "y": 220.5, "confidence": 0.98 },
    { "x": 160.2, "y": 225.7, "confidence": 0.97 },
    { "x": 140.0, "y": 250.0, "confidence": 0.95 },
    // ...more landmarks...
  ],
  "face_quality": {
    "is_good_face": true,
    "quality_score": 0.92,
    "is_frontal": true,
    "visible_landmarks": 5,
    "total_landmarks": 5,
    "avg_confidence": 0.96,
    "frontal_score": 0.85,
    "box_area_score": 0.8
  },
  "frame_number": 1234,
  "source_id": 0,
  "face_image": "<base64-encoded-jpeg>",
  "frame_image_path": "/app/outputs/frames/source0/frame_0001234.jpg"
}
```

## Troubleshooting

### Check if Kafka is running:
```bash
docker ps | grep kafka
```

### View Kafka logs:
```bash
docker logs kafka
docker logs kafka -f  # Follow logs
```

### Test connection from host:
```bash
telnet localhost 9092
```

### Access Kafka container shell:
```bash
docker exec -it kafka bash
```

## Install librdkafka 
```bash
apt-get install librdkafka-dev

pkg-config --cflags rdkafka

pkg-config --list-all | grep kafka


# Install other libs
# Install OpenCV
apt-get update
apt-get install libopencv-dev

pkg-config --cflags opencv4

# Install OpenSSL
apt-get install libssl-dev


# Build có Kafka support
make CUDA_VER=12.6 KAFKA=1


```
