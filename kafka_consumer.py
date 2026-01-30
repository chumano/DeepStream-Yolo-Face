from kafka import KafkaConsumer
from kafka.errors import KafkaError
import json
import sys

# Create consumer
try:
    consumer = KafkaConsumer(
        'face-detections',
        bootstrap_servers='localhost:9092',
        auto_offset_reset='earliest',  # Start from beginning
        enable_auto_commit=True,
        value_deserializer=lambda x: json.loads(x.decode('utf-8'))
    )
    print(f"Connected to Kafka broker: localhost:9092")
    print(f"Subscribed to topic: face-detections")
    print(f"Partitions: {consumer.partitions_for_topic('face-detections')}")
    print("Listening for face detection events...")
except KafkaError as e:
    print(f"ERROR - Failed to create consumer: {e}")
    sys.exit(1)

try:
    for message in consumer:
        detection = message.value
        print(f"\n--- New Face Detection ---")
        print(f"Partition: {message.partition}, Offset: {message.offset}")
        print(f"Object ID: {detection['object_id']}")
        print(f"Timestamp: {detection['timestamp']}")
        print(f"Confidence: {detection['confidence']:.2f}")
        print(f"BBox: [{detection['bbox']['left']:.0f}, {detection['bbox']['top']:.0f}, "
              f"{detection['bbox']['width']:.0f}, {detection['bbox']['height']:.0f}]")
        print(f"Frame: {detection['frame_number']}, Source: {detection['source_id']}")
except KeyboardInterrupt:
    print("\nStopping consumer...")
except Exception as e:
    print(f"ERROR - {e}")
finally:
    consumer.close()