from kafka import KafkaConsumer
from kafka.errors import KafkaError
import json
import sys
import time

# Landmark labels for face keypoints (typical 5-point configuration)
# Adjust based on your model's output format
LANDMARK_LABELS = {
    0: "Left Eye",
    1: "Right Eye",
    2: "Nose",
    3: "Left Mouth Corner",
    4: "Right Mouth Corner"
}

# Create consumer
try:
    consumer = KafkaConsumer(
        'face-detections',
        bootstrap_servers='localhost:9092',
        #auto_offset_reset='earliest',  # Start from beginning
        auto_offset_reset='latest',   # Start from latest
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
    # Initialize statistics
    message_count = 0
    start_time = time.time()
    last_stats_time = start_time
    stats_interval = 5  # Print stats every 5 seconds
    
    for message in consumer:
        message_count += 1
        current_time = time.time()
        
        detection = message.value
        print(f"\n--- New Face Detection ---")
        print(f"Partition: {message.partition}, Offset: {message.offset}")
        print(f"Object ID: {detection['object_id']}")
        print(f"Timestamp: {detection['timestamp']}")
        print(f"Confidence: {detection['confidence']:.2f}")
        print(f"BBox: [{detection['bbox']['left']:.0f}, {detection['bbox']['top']:.0f}, "
              f"{detection['bbox']['width']:.0f}, {detection['bbox']['height']:.0f}]")
        print(f"Frame: {detection['frame_number']}, Source: {detection['source_id']}")
        
        # Display face quality metrics
        if 'face_quality' in detection:
            quality = detection['face_quality']
            quality_status = "✓ GOOD" if quality.get('is_good_face', False) else "✗ POOR"
            frontal_status = "✓ FRONTAL" if quality.get('is_frontal', False) else "✗ NON-FRONTAL"
            
            print(f"\nFace Quality: {quality_status}")
            print(f"  Frontal Detection: {frontal_status}") 
            print(f"  Visible Landmarks: {quality.get('visible_landmarks', 0)}/{quality.get('total_landmarks', 0)}")
            print(f"  Avg Confidence: {quality.get('avg_confidence', 0):.3f}")
            print(f"  Overall Score: {quality.get('quality_score', 0):.3f}")
            print(f"  Frontal Score: {quality.get('frontal_score', 0):.3f}")
        
        # Display landmarks with labels
        if 'landmarks' in detection and detection['landmarks']:
            print(f"\nLandmarks ({len(detection['landmarks'])} points):")
            for idx, landmark in enumerate(detection['landmarks']):
                label = LANDMARK_LABELS.get(idx, f"Point {idx}")
                print(f"  {label}: x={landmark['x']:.1f}, y={landmark['y']:.1f}, conf={landmark['confidence']:.2f}")
        
        # Print statistics periodically
        elapsed_since_stats = current_time - last_stats_time
        if elapsed_since_stats >= stats_interval:
            total_elapsed = current_time - start_time
            avg_msg_per_sec = message_count / total_elapsed if total_elapsed > 0 else 0
            recent_msg_per_sec = (message_count - (message_count - stats_interval * avg_msg_per_sec)) / elapsed_since_stats if elapsed_since_stats > 0 else 0
            
            print(f"\n{'='*50}")
            print(f"STATISTICS:")
            print(f"  Total Messages: {message_count}")
            print(f"  Total Time: {total_elapsed:.1f}s")
            print(f"  Average Rate: {avg_msg_per_sec:.2f} msg/s")
            print(f"{'='*50}")
            last_stats_time = current_time
        
except KeyboardInterrupt:
    print("\nStopping consumer...")
    # Print final statistics
    total_time = time.time() - start_time
    if total_time > 0:
        avg_rate = message_count / total_time
        print(f"\nFINAL STATISTICS:")
        print(f"  Total Messages: {message_count}")
        print(f"  Total Time: {total_time:.1f}s")
        print(f"  Average Rate: {avg_rate:.2f} msg/s")
except Exception as e:
    print(f"ERROR - {e}")
finally:
    consumer.close()