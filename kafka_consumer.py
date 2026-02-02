from kafka import KafkaConsumer
from kafka.errors import KafkaError
import json
import sys
import time
import os
import base64
from datetime import datetime
import numpy as np
import cv2
import math

# Configuration
OUTPUT_DIR = "outputs/faces"  # Directory to save face images
SAVE_IMAGES = True  # Set to False to disable saving images

# Landmark labels for face keypoints (typical 5-point configuration)
# Adjust based on your model's output format
LANDMARK_LABELS = {
    0: "Left Eye",
    1: "Right Eye",
    2: "Nose",
    3: "Left Mouth Corner",
    4: "Right Mouth Corner"
}


def draw_landmarks_on_image(image, landmarks, bbox=None, color=(0, 255, 0), radius=2, thickness=-1):
    """
    Draw landmarks on an image.
    
    Args:
        image: BGR image (numpy array)
        landmarks: List of dicts with 'x', 'y' keys (in frame coordinates)
        bbox: Optional bounding box dict to convert coordinates (if landmarks are in frame coords)
        color: BGR color tuple for landmarks
        radius: Radius of landmark circles
        thickness: Thickness (-1 for filled circles)
    
    Returns:
        np.ndarray: Image with landmarks drawn
    """
    img_with_landmarks = image.copy()
    
    for idx, landmark in enumerate(landmarks):
        # Convert coordinates if bbox is provided
        if bbox is not None:
            x = int(landmark['x'] - bbox['left'])
            y = int(landmark['y'] - bbox['top'])
        else:
            x = int(landmark['x'])
            y = int(landmark['y'])
        
        # Draw landmark point
        cv2.circle(img_with_landmarks, (x, y), radius, color, thickness)
        
        # Draw label
        label = LANDMARK_LABELS.get(idx, str(idx))
        cv2.putText(img_with_landmarks, label, (x + 5, y - 5),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.3, color, 1, cv2.LINE_AA)
    
    return img_with_landmarks


def save_face_image(detection):
    """
    Save the face image from detection data to disk.
    
    Args:
        detection: Detection data dict containing face_image (base64)
    
    Returns:
        str: Path to saved image, or None if not saved
    """
    if not SAVE_IMAGES:
        return None
    
    face_image_base64 = detection.get('face_image')
    if not face_image_base64:
        return None
    
    try:
        # Create output directory if it doesn't exist
        os.makedirs(OUTPUT_DIR, exist_ok=True)
        
        # Decode base64 image
        image_data = base64.b64decode(face_image_base64)
        
        # Generate filename with timestamp and object ID
        timestamp = detection.get('timestamp', time.time())
        dt = datetime.fromtimestamp(timestamp)
        object_id = detection.get('object_id', 0)
        frame_num = detection.get('frame_number', 0)
        quality_score = detection.get('face_quality', {}).get('quality_score', 0)
        
        # Format: face_YYYYMMDD_HHMMSS_objID_frame_quality.jpg
        filename = f"face_{object_id:03d}_{dt.strftime('%Y%m%d_%H%M%S')}_f{frame_num}_q{quality_score:.3f}.jpg"
        filepath = os.path.join(OUTPUT_DIR, filename)
        
        # Save image
        with open(filepath, 'wb') as f:
            f.write(image_data)
        
        # Save version with landmarks drawn
        landmarks = detection.get('landmarks')
        bbox = detection.get('bbox')
        crop_bbox = detection.get('crop_bbox')  # Get crop bbox with padding
        if landmarks and bbox:
            img_array = np.frombuffer(image_data, np.uint8)
            img = cv2.imdecode(img_array, cv2.IMREAD_COLOR)
            if img is not None:
                # Use crop_bbox if available for coordinate conversion
                reference_bbox = crop_bbox if crop_bbox is not None else bbox
                img_with_landmarks = draw_landmarks_on_image(img, landmarks, reference_bbox)
                landmarks_filename = f"face_{object_id:03d}_{dt.strftime('%Y%m%d_%H%M%S')}_landmarks_f{frame_num}_q{quality_score:.3f}.jpg"
                landmarks_filepath = os.path.join(OUTPUT_DIR, landmarks_filename)
                cv2.imwrite(landmarks_filepath, img_with_landmarks)
        
        return filepath
    
    except Exception as e:
        print(f"ERROR - Failed to save face image: {e}")
        return None

def align_face(image_bytes, landmarks, bbox, target_size=(112, 112), desired_left_eye=(0.35, 0.35),):
    """
    Align a face in an image using eye landmarks.

    Args:
        image_bytes: Raw image bytes (BGR JPEG/PNG) - cropped face region.
        landmarks: List of 5 dicts with 'x', 'y' keys in full frame coordinates.
        bbox: Bounding box dict with 'left', 'top', 'width', 'height' keys.
        target_size: Output size (width, height).
        desired_left_eye: Normalized position of left eye in output image (x, y).

    Returns:
        np.ndarray: Aligned face image (BGR), or None if failed.
    """
    if len(landmarks) < 2:
        print("Face alignment requires at least 2 landmarks (eyes)")
        return None

    # Decode image bytes to BGR
    img_array = np.frombuffer(image_bytes, np.uint8)
    img = cv2.imdecode(img_array, cv2.IMREAD_COLOR)
    if img is None:
        print("Failed to decode image for alignment")
        return None

    # Convert landmarks from frame coordinates to face crop coordinates
    # Use crop_bbox if available (which includes padding), otherwise use bbox
    reference_bbox = bbox
    bbox_left = reference_bbox['left']
    bbox_top = reference_bbox['top']
    
    left_eye = (landmarks[0]['x'] - bbox_left, landmarks[0]['y'] - bbox_top)
    right_eye = (landmarks[1]['x'] - bbox_left, landmarks[1]['y'] - bbox_top)
    
    # Compute angle
    dx = right_eye[0] - left_eye[0]
    dy = right_eye[1] - left_eye[1]
    angle = math.degrees(math.atan2(dy, dx))

    # Compute scale
    desired_eye_distance = (1.0 - 2 * desired_left_eye[0]) * target_size[0]
    current_eye_distance = math.sqrt(dx * dx + dy * dy)
    scale = desired_eye_distance / current_eye_distance

    # Compute center between eyes
    eyes_center = (
        (left_eye[0] + right_eye[0]) / 2,
        (left_eye[1] + right_eye[1]) / 2
    )

    # Get rotation matrix
    M = cv2.getRotationMatrix2D(eyes_center, angle, scale)

    # Adjust translation
    tX = target_size[0] * 0.5
    tY = target_size[1] * desired_left_eye[1]
    M[0, 2] += (tX - eyes_center[0])
    M[1, 2] += (tY - eyes_center[1])

    # Warp image
    aligned = cv2.warpAffine(
        img, 
        M, 
        target_size, 
        flags=cv2.INTER_CUBIC,
        borderMode=cv2.BORDER_CONSTANT,
        borderValue=(0, 0, 0)
    )
    
    return aligned

def save_aligned_face_image(detection):
    """
    Align and save the face image from detection data to disk.

    Args:
        detection: Detection data dict containing face_image (base64) and landmarks

    Returns:
        str: Path to saved aligned image, or None if not saved
    """
    if not SAVE_IMAGES:
        return None

    face_image_base64 = detection.get('face_image')
    landmarks = detection.get('landmarks')
    bbox = detection.get('bbox')
    crop_bbox = detection.get('crop_bbox')  # Get crop bbox with padding
    
    if not face_image_base64 or not landmarks or len(landmarks) != 5 or not bbox:
        return None

    try:
        os.makedirs(OUTPUT_DIR, exist_ok=True)
        image_data = base64.b64decode(face_image_base64)
        reference_bbox = crop_bbox if crop_bbox is not None else bbox
        aligned = align_face(image_data, landmarks, reference_bbox, target_size=(112, 112))
        if aligned is None:
            return None

        timestamp = detection.get('timestamp', time.time())
        dt = datetime.fromtimestamp(timestamp)
        object_id = detection.get('object_id', 0)
        frame_num = detection.get('frame_number', 0)
        quality_score = detection.get('face_quality', {}).get('quality_score', 0)

        filename = f"face_{object_id:03d}_{dt.strftime('%Y%m%d_%H%M%S')}_aligned_f{frame_num}_q{quality_score:.3f}.jpg"
        filepath = os.path.join(OUTPUT_DIR, filename)
        cv2.imwrite(filepath, aligned)
        
        # Save version with transformed landmarks drawn for testing
        # Transform landmarks to aligned image coordinates
        reference_bbox = crop_bbox if crop_bbox is not None else bbox
        bbox_left = reference_bbox['left']
        bbox_top = reference_bbox['top']
        left_eye = (landmarks[0]['x'] - bbox_left, landmarks[0]['y'] - bbox_top)
        right_eye = (landmarks[1]['x'] - bbox_left, landmarks[1]['y'] - bbox_top)
        
        dx = right_eye[0] - left_eye[0]
        dy = right_eye[1] - left_eye[1]
        angle = math.degrees(math.atan2(dy, dx))
        
        desired_left_eye = (0.35, 0.35)
        desired_eye_distance = (1.0 - 2 * desired_left_eye[0]) * 112
        current_eye_distance = math.sqrt(dx * dx + dy * dy)
        scale = desired_eye_distance / current_eye_distance
        
        eyes_center = ((left_eye[0] + right_eye[0]) / 2, (left_eye[1] + right_eye[1]) / 2)
        M = cv2.getRotationMatrix2D(eyes_center, angle, scale)
        M[0, 2] += (112 * 0.5 - eyes_center[0])
        M[1, 2] += (112 * desired_left_eye[1] - eyes_center[1])
        
        # Transform all landmarks
        aligned_landmarks = []
        for lm in landmarks:
            x_crop = lm['x'] - bbox_left
            y_crop = lm['y'] - bbox_top
            x_aligned = M[0, 0] * x_crop + M[0, 1] * y_crop + M[0, 2]
            y_aligned = M[1, 0] * x_crop + M[1, 1] * y_crop + M[1, 2]
            aligned_landmarks.append({'x': x_aligned, 'y': y_aligned, 'confidence': lm['confidence']})
        
        # Draw landmarks on aligned image
        aligned_with_landmarks = draw_landmarks_on_image(aligned, aligned_landmarks, bbox=None)
        landmarks_filename = f"face_{object_id:03d}_{dt.strftime('%Y%m%d_%H%M%S')}_aligned_landmarks_f{frame_num}_q{quality_score:.3f}.jpg"
        landmarks_filepath = os.path.join(OUTPUT_DIR, landmarks_filename)
        cv2.imwrite(landmarks_filepath, aligned_with_landmarks)
        
        return filepath

    except Exception as e:
        print(f"ERROR - Failed to save aligned face image: {e}")
        return None


# Create consumer
try:
    # clear existed output directory
    if SAVE_IMAGES and os.path.exists(OUTPUT_DIR):
        for f in os.listdir(OUTPUT_DIR):
            os.remove(os.path.join(OUTPUT_DIR, f))

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
    print(f"Saving images to: {os.path.abspath(OUTPUT_DIR)}" if SAVE_IMAGES else "Image saving disabled")
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
    images_saved = 0  # Track saved images count
    
    for message in consumer:
        message_count += 1
        current_time = time.time()
        
        detection = message.value
        print(f"\n--- New Face Detection ---")
        print(f"Partition: {message.partition}, Offset: {message.offset}")
        print(f"Object ID: {detection['object_id']}")
        print(f"Timestamp: {detection['timestamp']}")
        print(f"Confidence: {detection['confidence']:.2f}")
        print(f"Frame Size: {detection.get('frame_size', {})}")
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
            print(f"  Overall Score: {quality.get('quality_score', 0):.3f}")
            print(f"  Avg Confidence: {quality.get('avg_confidence', 0):.3f}")
            print(f"  Frontal Score: {quality.get('frontal_score', 0):.3f}")
            print(f"  Area Score: {quality.get('box_area_score', 0):.3f}")
        
        # Display landmarks with labels
        if 'landmarks' in detection and detection['landmarks']:
            print(f"\nLandmarks ({len(detection['landmarks'])} points):")
            for idx, landmark in enumerate(detection['landmarks']):
                label = LANDMARK_LABELS.get(idx, f"Point {idx}")
                print(f"  {label}: x={landmark['x']:.1f}, y={landmark['y']:.1f}, conf={landmark['confidence']:.2f}")
        
        # Save face image if present
        if detection.get('face_image'):
            saved_path = save_face_image(detection)
            if saved_path:
                images_saved += 1
                print(f"\n📷 Face image saved: {saved_path}")
            else:
                print(f"\n⚠️ Face image present but failed to save")
            # Save aligned face image if possible
            aligned_path = save_aligned_face_image(detection)
            if aligned_path:
                print(f"🧑‍🎤 Aligned face image saved: {aligned_path}")
            elif detection.get('landmarks') and len(detection['landmarks']) == 5:
                print(f"⚠️ Failed to align face image")
        else:
            print(f"\n⚠️ No face image in detection")
        
        # Print statistics periodically
        elapsed_since_stats = current_time - last_stats_time
        if elapsed_since_stats >= stats_interval:
            total_elapsed = current_time - start_time
            avg_msg_per_sec = message_count / total_elapsed if total_elapsed > 0 else 0
            recent_msg_per_sec = (message_count - (message_count - stats_interval * avg_msg_per_sec)) / elapsed_since_stats if elapsed_since_stats > 0 else 0
            
            print(f"\n{'='*50}")
            print(f"STATISTICS:")
            print(f"  Total Messages: {message_count}")
            print(f"  Images Saved: {images_saved}")
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
        print(f"  Images Saved: {images_saved}")
        print(f"  Total Time: {total_time:.1f}s")
        print(f"  Average Rate: {avg_rate:.2f} msg/s")
except Exception as e:
    print(f"ERROR - {e}")
finally:
    consumer.close()