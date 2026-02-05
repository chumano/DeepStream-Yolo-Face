import json
import sys
import time
import os
import base64
import logging
import math
from typing import Optional, Dict, List, Tuple, Any
from datetime import datetime
from dataclasses import dataclass, field

import numpy as np
import cv2
import requests

# pip install kafka-python==2.3.0
from kafka import KafkaConsumer
from kafka.errors import KafkaError

# pip install qdrant-client==1.16.2
from qdrant_client import QdrantClient
from qdrant_client.models import PointStruct, VectorParams, Distance

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s'
    #format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)


@dataclass
class Config:
    """Application configuration."""
    output_dir: str = field(default_factory=lambda: os.getenv("OUTPUT_DIR", "outputs/faces"))
    save_images: bool = field(default_factory=lambda: os.getenv("SAVE_IMAGES", "True").lower() in ("1", "true", "yes"))
    save_aligned: bool = field(default_factory=lambda: os.getenv("SAVE_ALIGNED", "True").lower() in ("1", "true", "yes"))
    save_landmarks: bool = field(default_factory=lambda: os.getenv("SAVE_LANDMARKS", "False").lower() in ("1", "true", "yes"))
    qdrant_url: str = field(default_factory=lambda: os.getenv("QDRANT_URL", "http://localhost:6333"))
    qdrant_collection: str = field(default_factory=lambda: os.getenv("QDRANT_COLLECTION", "faces"))
    embed_url: str = field(default_factory=lambda: os.getenv("EMBED_URL", "http://localhost:5000/embed"))
    embedding_size: int = 512
    kafka_bootstrap_servers: str = field(default_factory=lambda: os.getenv("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092"))
    kafka_topic: str = field(default_factory=lambda: os.getenv("KAFKA_TOPIC", "face-detections"))
    kafka_offset_reset: str = field(default_factory=lambda: os.getenv("KAFKA_OFFSET_RESET", "latest"))
    stats_interval: int = field(default_factory=lambda: int(os.getenv("STATS_INTERVAL", 5)))
    target_face_size: Tuple[int, int] = (112, 112)
    desired_left_eye: Tuple[float, float] = (0.35, 0.35)
    landmark_labels: Dict[int, str] = field(default_factory=lambda: {
        0: "Left Eye",
        1: "Right Eye",
        2: "Nose",
        3: "Left Mouth Corner",
        4: "Right Mouth Corner"
    })


class ImageProcessor:
    """Handles image processing operations."""
    
    def __init__(self, config: Config):
        self.config = config
    
    def decode_image(self, image_data: bytes) -> Optional[np.ndarray]:
        """Decode image bytes to numpy array."""
        try:
            img_array = np.frombuffer(image_data, np.uint8)
            return cv2.imdecode(img_array, cv2.IMREAD_COLOR)
        except Exception as e:
            logger.error(f"Failed to decode image: {e}")
            return None
    
    def draw_landmarks(
        self,
        image: np.ndarray,
        landmarks: List[Dict[str, float]],
        bbox: Optional[Dict[str, float]] = None,
        color: Tuple[int, int, int] = (0, 255, 0),
        radius: int = 2,
        thickness: int = -1
    ) -> np.ndarray:
        """Draw landmarks on image."""
        img_with_landmarks = image.copy()
        
        for idx, landmark in enumerate(landmarks):
            x = int(landmark['x'] - (bbox['left'] if bbox else 0))
            y = int(landmark['y'] - (bbox['top'] if bbox else 0))
            
            cv2.circle(img_with_landmarks, (x, y), radius, color, thickness)
            
            label = self.config.landmark_labels.get(idx, str(idx))
            cv2.putText(
                img_with_landmarks, label, (x + 5, y - 5),
                cv2.FONT_HERSHEY_SIMPLEX, 0.3, color, 1, cv2.LINE_AA
            )
        
        return img_with_landmarks
    
    def align_face(
        self,
        image: np.ndarray,
        landmarks: List[Dict[str, float]],
        bbox: Dict[str, float]
    ) -> Optional[np.ndarray]:
        """Align face using eye landmarks."""
        if len(landmarks) < 2:
            logger.warning("Face alignment requires at least 2 landmarks")
            return None
        
        bbox_left = bbox['left']
        bbox_top = bbox['top']
        
        left_eye = (landmarks[0]['x'] - bbox_left, landmarks[0]['y'] - bbox_top)
        right_eye = (landmarks[1]['x'] - bbox_left, landmarks[1]['y'] - bbox_top)
        
        dx = right_eye[0] - left_eye[0]
        dy = right_eye[1] - left_eye[1]
        angle = math.degrees(math.atan2(dy, dx))
        
        desired_eye_distance = (1.0 - 2 * self.config.desired_left_eye[0]) * self.config.target_face_size[0]
        current_eye_distance = math.sqrt(dx * dx + dy * dy)
        
        if current_eye_distance == 0:
            logger.warning("Invalid eye distance for alignment")
            return None
        
        scale = desired_eye_distance / current_eye_distance
        eyes_center = ((left_eye[0] + right_eye[0]) / 2, (left_eye[1] + right_eye[1]) / 2)
        
        M = cv2.getRotationMatrix2D(eyes_center, angle, scale)
        M[0, 2] += (self.config.target_face_size[0] * 0.5 - eyes_center[0])
        M[1, 2] += (self.config.target_face_size[1] * self.config.desired_left_eye[1] - eyes_center[1])
        
        aligned = cv2.warpAffine(
            image, M, self.config.target_face_size,
            flags=cv2.INTER_CUBIC,
            borderMode=cv2.BORDER_CONSTANT,
            borderValue=(0, 0, 0)
        )
        
        return aligned
    
    def align_face_from_detection(
        self,
        detection: Dict[str, Any]
    ) -> Tuple[Optional[np.ndarray], Optional[bytes]]:
        """Align face image from detection data and return aligned image array and bytes."""
        face_image_base64 = detection.get('face_image')
        landmarks = detection.get('landmarks')
        bbox = detection.get('bbox')
        crop_bbox = detection.get('crop_bbox')
        
        if not face_image_base64 or not landmarks or len(landmarks) != 5 or not bbox:
            return None, None
        
        try:
            image_data = base64.b64decode(face_image_base64)
            img = self.decode_image(image_data)
            if img is None:
                return None, None
            
            reference_bbox = crop_bbox if crop_bbox else bbox
            aligned = self.align_face(img, landmarks, reference_bbox)
            if aligned is None:
                return None, None
            
            _, aligned_bytes = cv2.imencode('.jpg', aligned)
            return aligned, aligned_bytes.tobytes()
        
        except Exception as e:
            logger.error(f"Failed to align face: {e}")
            return None, None


class FaceStorage:
    """Handles face image storage operations."""
    
    def __init__(self, config: Config):
        self.config = config
        self.image_processor = ImageProcessor(config)
        self._ensure_output_dir()
    
    def _ensure_output_dir(self) -> None:
        """Create output directory if needed."""
        if self.config.save_images:
            os.makedirs(self.config.output_dir, exist_ok=True)
    
    def _generate_filename(
        self,
        detection: Dict[str, Any],
        prefix: str = "face",
        suffix: str = ""
    ) -> str:
        """Generate filename from detection metadata."""
        timestamp = detection.get('timestamp', time.time())
        dt = datetime.fromtimestamp(timestamp)
        object_id = detection.get('object_id', 0)
        frame_num = detection.get('frame_number', 0)
        quality_score = detection.get('face_quality', {}).get('quality_score', 0)
        
        filename = f"{prefix}_{object_id:03d}_{dt.strftime('%Y%m%d_%H%M%S')}"
        filename += f"_f{frame_num}_q{quality_score:.3f}"
        if suffix:
            filename += f"_{suffix}"
        filename += ".jpg"
        
        return os.path.join(self.config.output_dir, filename)
    
    def save_face_image(self, detection: Dict[str, Any]) -> Optional[str]:
        """Save raw face image."""
        if not self.config.save_images:
            return None
        
        face_image_base64 = detection.get('face_image')
        if not face_image_base64:
            return None
        
        try:
            image_data = base64.b64decode(face_image_base64)
            filepath = self._generate_filename(detection)
            
            with open(filepath, 'wb') as f:
                f.write(image_data)
            
            # Optionally save version with landmarks
            if self.config.save_landmarks:
                self._save_with_landmarks(detection, image_data)
            
            return filepath
        
        except Exception as e:
            logger.error(f"Failed to save face image: {e}")
            return None
    
    def _save_with_landmarks(self, detection: Dict[str, Any], image_data: bytes) -> None:
        """Save image with landmarks drawn."""
        landmarks = detection.get('landmarks')
        bbox = detection.get('bbox')
        crop_bbox = detection.get('crop_bbox')
        
        if not landmarks or not bbox:
            return
        
        img = self.image_processor.decode_image(image_data)
        if img is None:
            return
        
        reference_bbox = crop_bbox if crop_bbox else bbox
        img_with_landmarks = self.image_processor.draw_landmarks(img, landmarks, reference_bbox)
        
        filepath = self._generate_filename(detection, suffix="landmarks")
        cv2.imwrite(filepath, img_with_landmarks)
    
    def save_aligned_image(
        self,
        aligned: np.ndarray,
        detection: Dict[str, Any]
    ) -> Optional[str]:
        """Save aligned face image to disk."""
        if not self.config.save_aligned:
            return None
        
        try:
            filepath = self._generate_filename(detection, suffix="aligned")
            cv2.imwrite(filepath, aligned)
            return filepath
        except Exception as e:
            logger.error(f"Failed to save aligned image: {e}")
            return None
    
    def save_aligned_with_landmarks(
        self,
        aligned: np.ndarray,
        detection: Dict[str, Any]
    ) -> Optional[str]:
        """Save aligned face image with landmarks drawn."""
        if not self.config.save_landmarks:
            return None
        
        landmarks = detection.get('landmarks')
        bbox = detection.get('bbox')
        crop_bbox = detection.get('crop_bbox')
        
        if not landmarks or not bbox:
            return None
        
        try:
            reference_bbox = crop_bbox if crop_bbox else bbox
            transformed_landmarks = self._transform_landmarks(landmarks, reference_bbox)
            aligned_with_landmarks = self.image_processor.draw_landmarks(
                aligned, transformed_landmarks, bbox=None
            )
            filepath = self._generate_filename(detection, suffix="aligned_landmarks")
            cv2.imwrite(filepath, aligned_with_landmarks)
            return filepath
        except Exception as e:
            logger.warning(f"Failed to save aligned landmarks: {e}")
            return None
    
    
    def _transform_landmarks(
        self,
        landmarks: List[Dict[str, float]],
        reference_bbox: Dict[str, float]
    ) -> List[Dict[str, float]]:
        """Transform landmarks to aligned image coordinates."""
        bbox_left = reference_bbox['left']
        bbox_top = reference_bbox['top']
        
        left_eye = (landmarks[0]['x'] - bbox_left, landmarks[0]['y'] - bbox_top)
        right_eye = (landmarks[1]['x'] - bbox_left, landmarks[1]['y'] - bbox_top)
        
        dx = right_eye[0] - left_eye[0]
        dy = right_eye[1] - left_eye[1]
        angle = math.degrees(math.atan2(dy, dx))
        
        desired_eye_distance = (1.0 - 2 * self.config.desired_left_eye[0]) * self.config.target_face_size[0]
        current_eye_distance = math.sqrt(dx * dx + dy * dy)
        scale = desired_eye_distance / current_eye_distance if current_eye_distance > 0 else 1.0
        
        eyes_center = ((left_eye[0] + right_eye[0]) / 2, (left_eye[1] + right_eye[1]) / 2)
        M = cv2.getRotationMatrix2D(eyes_center, angle, scale)
        M[0, 2] += (self.config.target_face_size[0] * 0.5 - eyes_center[0])
        M[1, 2] += (self.config.target_face_size[1] * self.config.desired_left_eye[1] - eyes_center[1])
        
        transformed = []
        for lm in landmarks:
            x_crop = lm['x'] - bbox_left
            y_crop = lm['y'] - bbox_top
            x_aligned = M[0, 0] * x_crop + M[0, 1] * y_crop + M[0, 2]
            y_aligned = M[1, 0] * x_crop + M[1, 1] * y_crop + M[1, 2]
            transformed.append({
                'x': x_aligned,
                'y': y_aligned,
                'confidence': lm['confidence']
            })
        
        return transformed


class EmbeddingService:
    """Handles face embedding generation and storage."""
    
    def __init__(self, config: Config):
        self.config = config
        self.qdrant_client = QdrantClient(url=config.qdrant_url)
        self._ensure_collection()
    
    def _ensure_collection(self) -> None:
        """Ensure Qdrant collection exists."""
        try:
            collections = self.qdrant_client.get_collections().collections
            collection_names = [c.name for c in collections]
            
            if self.config.qdrant_collection not in collection_names:
                self.qdrant_client.create_collection(
                    collection_name=self.config.qdrant_collection,
                    vectors_config=VectorParams(
                        size=self.config.embedding_size,
                        distance=Distance.COSINE
                    )
                )
                logger.info(f"Created Qdrant collection: {self.config.qdrant_collection}")
            else:
                logger.info(f"Using existing Qdrant collection: {self.config.qdrant_collection}")
        except Exception as e:
            logger.error(f"Failed to setup Qdrant collection: {e}")
            raise
    
    def generate_embedding(self, image_bytes: bytes) -> Tuple[float, Optional[List[float]]]:
        """Generate embedding from image bytes."""
        try:
            files = {"image": ("face.jpg", image_bytes, "image/jpeg")}
            start = time.time()
            response = requests.post(self.config.embed_url, files=files, timeout=10)
            elapsed = time.time() - start
            
            if response.status_code == 200:
                result = response.json()
                if result.get('success'):
                    return elapsed, result.get('embedding')
            
            logger.error(f"Embedding request failed: {response.status_code}")
            return elapsed, None
        
        except Exception as e:
            logger.error(f"Failed to generate embedding: {e}")
            return 0, None
    
    def save_embedding(
        self,
        embedding: List[float],
        detection: Dict[str, Any],
        aligned_filepath: Optional[str]
    ) -> Optional[str]:
        """Save embedding to Qdrant."""
        try:
            object_id = detection.get('object_id', 0)
            timestamp = detection.get('timestamp', time.time())
            frame_num = detection.get('frame_number', 0)
            quality_score = detection.get('face_quality', {}).get('quality_score', 0)
            
            point_id_str = f"{object_id}_{int(timestamp * 1000)}_{frame_num}"
            
            payload = {
                "object_id": object_id,
                "timestamp": timestamp,
                "frame_number": frame_num,
                "source_id": detection.get('source_id', ''),
                "confidence": detection.get('confidence', 0),
                "quality_score": quality_score,
                "is_frontal": detection.get('face_quality', {}).get('is_frontal', False),
                "bbox": detection.get('bbox', {}),
                "aligned_image_path": aligned_filepath or "",
                "datetime": datetime.fromtimestamp(timestamp).isoformat()
            }
            
            point = PointStruct(
                id=hash(point_id_str) & 0x7FFFFFFFFFFFFFFF,
                vector=embedding,
                payload=payload
            )
            
            self.qdrant_client.upsert(
                collection_name=self.config.qdrant_collection,
                points=[point]
            )
            
            return point_id_str
        
        except Exception as e:
            logger.error(f"Failed to save embedding to Qdrant: {e}")
            return None


class DetectionPrinter:
    """Handles printing detection information."""
    
    def __init__(self, config: Config):
        self.config = config
    
    def print_detection(self, detection: Dict[str, Any], partition: int, offset: int) -> None:
        """Print detection information."""
        logger.info("\n--- New Face Detection ---")
        logger.info(f"Partition: {partition}, Offset: {offset}")
        logger.info(f"Object ID: {detection['object_id']}")
        # Format timestamp to human-readable string with milliseconds
        ts = detection['timestamp']
        try:
            ts_human = datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
        except Exception:
            ts_human = str(ts)
        logger.info(f"Timestamp: {ts} ({ts_human})")
        logger.info(f"Confidence: {detection['confidence']:.2f}")
        logger.info(f"Frame Size: {detection.get('frame_size', {})}")
        
        bbox = detection['bbox']
        logger.info(
            f"BBox: [{bbox['left']:.0f}, {bbox['top']:.0f}, "
            f"{bbox['width']:.0f}, {bbox['height']:.0f}]"
        )
        logger.info(f"Frame: {detection['frame_number']}, Source: {detection['source_id']}")
        
        self._print_quality_info(detection)
        self._print_landmarks(detection)
    
    def _print_quality_info(self, detection: Dict[str, Any]) -> None:
        """Print face quality information."""
        if 'face_quality' not in detection:
            return
        
        quality = detection['face_quality']
        quality_status = "✓ GOOD" if quality.get('is_good_face', False) else "✗ POOR"
        frontal_status = "✓ FRONTAL" if quality.get('is_frontal', False) else "✗ NON-FRONTAL"
        
        logger.info(f"\nFace Quality: {quality_status}")
        logger.info(f"  Frontal Detection: {frontal_status}")
        logger.info(
            f"  Visible Landmarks: {quality.get('visible_landmarks', 0)}/"
            f"{quality.get('total_landmarks', 0)}"
        )
        logger.info(f"  Overall Score: {quality.get('quality_score', 0):.3f}")
        logger.info(f"  Avg Confidence: {quality.get('avg_confidence', 0):.3f}")
        logger.info(f"  Frontal Score: {quality.get('frontal_score', 0):.3f}")
        logger.info(f"  Area Score: {quality.get('box_area_score', 0):.3f}")
    
    def _print_landmarks(self, detection: Dict[str, Any]) -> None:
        """Print landmark information."""
        landmarks = detection.get('landmarks', [])
        if not landmarks:
            return
        
        logger.info(f"\nLandmarks ({len(landmarks)} points):")
        for idx, landmark in enumerate(landmarks):
            label = self.config.landmark_labels.get(idx, f"Point {idx}")
            logger.info(
                f"  {label}: x={landmark['x']:.1f}, y={landmark['y']:.1f}, "
                f"conf={landmark['confidence']:.2f}"
            )


class FaceDetectionConsumer:
    """Main consumer class for face detections."""
    
    def __init__(self, config: Config):
        self.config = config
        self.storage = FaceStorage(config)
        self.embedding_service = EmbeddingService(config)
        self.printer = DetectionPrinter(config)
        self.consumer: Optional[KafkaConsumer] = None
        
        # Statistics
        self.message_count = 0
        self.images_saved = 0
        self.embeddings_saved = 0
        self.start_time = time.time()
        self.last_stats_time = self.start_time
    
    def _create_consumer(self) -> KafkaConsumer:
        """Create and configure Kafka consumer."""
        return KafkaConsumer(
            self.config.kafka_topic,
            bootstrap_servers=self.config.kafka_bootstrap_servers,
            auto_offset_reset=self.config.kafka_offset_reset,
            enable_auto_commit=True,
            value_deserializer=lambda x: json.loads(x.decode('utf-8'))
        )
    
    def _cleanup_output_dir(self) -> None:
        """Clean up existing output directory."""
        if self.config.save_images and os.path.exists(self.config.output_dir):
            for f in os.listdir(self.config.output_dir):
                try:
                    os.remove(os.path.join(self.config.output_dir, f))
                except Exception as e:
                    logger.warning(f"Failed to remove {f}: {e}")
    
    def start(self) -> None:
        """Start consuming messages."""
        try:
            self._cleanup_output_dir()
            self.consumer = self._create_consumer()
            
            logger.info(f"Connected to Kafka broker: {self.config.kafka_bootstrap_servers}")
            logger.info(f"Subscribed to topic: {self.config.kafka_topic}")
            logger.info(f"Partitions: {self.consumer.partitions_for_topic(self.config.kafka_topic)}")
            
            if self.config.save_images:
                logger.info(f"Saving images to: {os.path.abspath(self.config.output_dir)}")
                logger.info(f"  - Raw images: enabled")
                logger.info(f"  - Aligned images: {'enabled' if self.config.save_aligned else 'disabled'}")
                logger.info(f"  - Landmarks overlay: {'enabled' if self.config.save_landmarks else 'disabled'}")
            else:
                logger.info("Image saving disabled")
            
            logger.info("Listening for face detection events...")
            
            self._consume_messages()
        
        except KafkaError as e:
            logger.error(f"Kafka error: {e}")
            sys.exit(1)
        except KeyboardInterrupt:
            logger.info("\nStopping consumer...")
            self._print_final_statistics()
        except Exception as e:
            logger.error(f"Unexpected error: {e}", exc_info=True)
        finally:
            if self.consumer:
                self.consumer.close()
    
    def _consume_messages(self) -> None:
        """Main message consumption loop."""
        for message in self.consumer:
            try:
                self._process_message(message)
                self._maybe_print_statistics()
            except Exception as e:
                logger.error(f"Error processing message: {e}", exc_info=True)
    
    def _process_message(self, message: Any) -> None:
        """Process a single message."""
        self.message_count += 1
        detection = message.value
        
        self.printer.print_detection(detection, message.partition, message.offset)
        
        # Save raw face image
        if detection.get('face_image'):
            saved_path = self.storage.save_face_image(detection)
            if saved_path:
                self.images_saved += 1
                logger.info(f"\n📷 Face image saved: {saved_path}")
            
            # Save aligned face and generate embedding
            self._process_aligned_face(detection)
    
    def _process_aligned_face(self, detection: Dict[str, Any]) -> None:
        """Process aligned face and generate embedding."""
        # Step 1: Align the face image
        aligned, aligned_bytes = self.storage.image_processor.align_face_from_detection(detection)
        
        if aligned is None:
            if detection.get('landmarks') and len(detection['landmarks']) == 5:
                logger.warning("⚠️ Failed to align face image")
            return
        
        # Step 2: Save aligned image (if configured)
        aligned_path = None
        if self.config.save_aligned:
            aligned_path = self.storage.save_aligned_image(aligned, detection)
            if aligned_path:
                logger.info(f"🧑‍🎤 Aligned face image saved: {aligned_path}")
        
        # Step 3: Save aligned image with landmarks (if configured)
        if self.config.save_landmarks:
            landmarks_path = self.storage.save_aligned_with_landmarks(aligned, detection)
            if landmarks_path:
                logger.info(f"🎯 Aligned landmarks saved: {landmarks_path}")
        
        # Step 4: Generate embedding
        if not aligned_bytes:
            logger.warning("⚠️ No aligned image bytes available for embedding")
            return
        
        elapsed, embedding = self.embedding_service.generate_embedding(aligned_bytes)
        
        if not embedding:
            logger.error("❌ Failed to generate embedding")
            return
        
        # Step 5: Save embedding to Qdrant
        point_id = self.embedding_service.save_embedding(embedding, detection, aligned_path)
        
        if point_id:
            self.embeddings_saved += 1
            logger.info(f"💾 Embedding saved to Qdrant (ID: {point_id}, time: {elapsed:.3f}s)")
        else:
            logger.error("❌ Failed to save embedding to Qdrant")
    
    def _maybe_print_statistics(self) -> None:
        """Print statistics if interval elapsed."""
        current_time = time.time()
        elapsed_since_stats = current_time - self.last_stats_time
        
        if elapsed_since_stats >= self.config.stats_interval:
            total_elapsed = current_time - self.start_time
            avg_msg_per_sec = self.message_count / total_elapsed if total_elapsed > 0 else 0
            
            logger.info(f"\n{'='*50}")
            logger.info("STATISTICS:")
            logger.info(f"  Total Messages: {self.message_count}")
            logger.info(f"  Images Saved: {self.images_saved}")
            logger.info(f"  Embeddings Saved: {self.embeddings_saved}")
            logger.info(f"  Total Time: {total_elapsed:.1f}s")
            logger.info(f"  Average Rate: {avg_msg_per_sec:.2f} msg/s")
            logger.info(f"{'='*50}")
            
            self.last_stats_time = current_time
    
    def _print_final_statistics(self) -> None:
        """Print final statistics."""
        total_time = time.time() - self.start_time
        
        if total_time > 0:
            avg_rate = self.message_count / total_time
            logger.info("\nFINAL STATISTICS:")
            logger.info(f"  Total Messages: {self.message_count}")
            logger.info(f"  Images Saved: {self.images_saved}")
            logger.info(f"  Embeddings Saved: {self.embeddings_saved}")
            logger.info(f"  Total Time: {total_time:.1f}s")
            logger.info(f"  Average Rate: {avg_rate:.2f} msg/s")


def main():
    """Main entry point."""
    config = Config()
    consumer = FaceDetectionConsumer(config)
    consumer.start()


if __name__ == "__main__":
    main()