import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib

import os
import sys
import time
import argparse
import platform
import json
import base64
from abc import ABC, abstractmethod
from threading import Lock
from ctypes import sizeof, c_float
from collections import deque
from typing import Dict, Optional, Any, List, Tuple
from dataclasses import dataclass
from enum import Enum
import math
import cv2
import numpy as np

from kafka import KafkaProducer
from kafka.errors import KafkaError

sys.path.append("/opt/nvidia/deepstream/deepstream/lib")
import pyds


# =============================================================================
# Detection Manager Classes
# =============================================================================

class SendResult(Enum):
    """Result of a send operation"""
    SUCCESS = "success"
    SKIPPED = "skipped"
    FAILED = "failed"
    PENDING = "pending"


@dataclass
class Detection:
    """Represents a face detection with metadata"""
    object_id: int
    detection_data: Dict[str, Any]
    quality_score: float
    timestamp: float
    is_resend: bool = False
    
    def to_dict(self) -> Dict[str, Any]:
        return self.detection_data


@dataclass
class DetectionRecord:
    """Record of a sent detection"""
    object_id: int
    quality_score: float
    sent_timestamp: float
    send_count: int = 1
    last_seen_timestamp: float = 0.0  # Track when object was last seen
    
    def __post_init__(self):
        if self.last_seen_timestamp == 0.0:
            self.last_seen_timestamp = self.sent_timestamp


class SendStrategy(ABC):
    """Abstract base class for detection sending strategies"""
    
    @abstractmethod
    def should_queue(self, detection: Detection, 
                     pending: Optional[Detection],
                     sent_record: Optional[DetectionRecord]) -> Tuple[bool, str]:
        pass
    
    @abstractmethod
    def should_send(self, detection: Detection, current_time: float) -> Tuple[bool, str]:
        pass
    
    @abstractmethod
    def on_new_detection(self, detection: Detection,
                         pending: Optional[Detection]) -> Optional[Detection]:
        pass


class DelayedBestQualitySendStrategy(SendStrategy):
    """
    Strategy that waits for a configurable delay and sends the best quality detection.
    Allows resending if quality improves significantly.
    """
    
    def __init__(self, delay_sec: float = 2.0, quality_improvement_threshold: float = 0.1):
        self.delay_sec = delay_sec
        self.quality_improvement_threshold = quality_improvement_threshold
    
    def should_queue(self, detection: Detection,
                     pending: Optional[Detection],
                     sent_record: Optional[DetectionRecord]) -> Tuple[bool, str]:
        # If already sent, only allow if quality improves significantly
        if sent_record is not None:
            improvement = detection.quality_score - sent_record.quality_score
            # Reject if quality is worse or improvement is insufficient
            if improvement < self.quality_improvement_threshold:
                return False, f"insufficient improvement ({improvement:.3f} < {self.quality_improvement_threshold})"
        
        # If there's a pending detection, only replace if new one is better
        if pending is not None:
            if detection.quality_score <= pending.quality_score:
                return False, f"pending has better quality ({pending.quality_score:.3f} >= {detection.quality_score:.3f})"
            # Also check: if we have both sent_record and pending, ensure new detection
            # is still better than sent_record (pending might have been queued before sent)
            if sent_record is not None and detection.quality_score <= sent_record.quality_score:
                return False, f"not better than already sent ({sent_record.quality_score:.3f})"
        
        if detection.detection_data.get("face_quality", {}).get("is_good_face") is not True:
            return False, "face quality not good"

        return True, "queued for delayed send"
    
    def should_send(self, detection: Detection, current_time: float) -> Tuple[bool, str]:
        elapsed = current_time - detection.timestamp
        if elapsed >= self.delay_sec:
            return True, f"delay elapsed ({elapsed:.2f}s >= {self.delay_sec}s)"
        return False, f"waiting ({elapsed:.2f}s < {self.delay_sec}s)"
    
    def on_new_detection(self, detection: Detection,
                         pending: Optional[Detection]) -> Optional[Detection]:
        if pending is not None:
            # Giữ timestamp gốc để delay được tính từ detection đầu tiên
            detection.timestamp = pending.timestamp
            # Giữ is_resend flag từ detection gốc
            detection.is_resend = pending.is_resend
        return detection


class ImmediateSendStrategy(SendStrategy):
    """Strategy that sends detections immediately without delay."""
    
    def __init__(self, quality_improvement_threshold: float = 0.1):
        self.quality_improvement_threshold = quality_improvement_threshold
    
    def should_queue(self, detection: Detection,
                     pending: Optional[Detection],
                     sent_record: Optional[DetectionRecord]) -> Tuple[bool, str]:
        if sent_record is not None:
            improvement = detection.quality_score - sent_record.quality_score
            if improvement < self.quality_improvement_threshold:
                return False, f"already sent with similar quality"
        return True, "queued for immediate send"
    
    def should_send(self, detection: Detection, current_time: float) -> Tuple[bool, str]:
        return True, "immediate send"
    
    def on_new_detection(self, detection: Detection,
                         pending: Optional[Detection]) -> Optional[Detection]:
        return detection


class KafkaSender:
    """Handles Kafka connection and message sending"""
    
    def __init__(self, broker: str, topic: str):
        self.broker = broker
        self.topic = topic
        self.producer: Optional[KafkaProducer] = None
    
    def connect(self) -> bool:
        try:
            self.producer = KafkaProducer(
                bootstrap_servers=self.broker,
                value_serializer=lambda v: json.dumps(v).encode('utf-8'),
                acks=1,
                compression_type='gzip',
                linger_ms=10,
                request_timeout_ms=30000,
                retries=3
            )
            self.producer.bootstrap_connected()
            sys.stdout.write(f"INFO - Kafka producer initialized (broker: {self.broker}, topic: {self.topic})\n")
            return True
        except Exception as e:
            sys.stderr.write(f"ERROR - Failed to initialize Kafka producer: {e}\n")
            self.producer = None
            return False
    
    def send(self, data: Dict[str, Any], timeout: float = 1.0) -> SendResult:
        if self.producer is None:
            return SendResult.FAILED
        
        try:
            future = self.producer.send(self.topic, value=data)
            future.get(timeout=timeout)
            return SendResult.SUCCESS
        except KafkaError as e:
            sys.stderr.write(f"ERROR - Failed to send to Kafka: {e}\n")
            return SendResult.FAILED
        except Exception as e:
            sys.stderr.write(f"ERROR - Unexpected error sending to Kafka: {e}\n")
            return SendResult.FAILED
    
    def close(self):
        if self.producer is not None:
            try:
                self.producer.flush()
                self.producer.close()
                sys.stdout.write("INFO - Kafka producer closed\n")
            except Exception as e:
                sys.stderr.write(f"ERROR - Failed to close Kafka producer: {e}\n")


class DetectionStore:
    """Thread-safe storage for pending and sent detections"""
    
    def __init__(self, sent_record_ttl_sec: float = 60.0, pending_ttl_sec: float = 10.0):
        self._pending: Dict[int, Detection] = {}
        self._sent: Dict[int, DetectionRecord] = {}
        self._pending_lock = Lock()
        self._sent_lock = Lock()
        self.sent_record_ttl_sec = sent_record_ttl_sec  # TTL for sent records
        self.pending_ttl_sec = pending_ttl_sec  # TTL for pending detections
    
    def get_pending(self, object_id: int) -> Optional[Detection]:
        with self._pending_lock:
            return self._pending.get(object_id)
    
    def set_pending(self, detection: Detection):
        with self._pending_lock:
            self._pending[detection.object_id] = detection
    
    def remove_pending(self, object_id: int) -> Optional[Detection]:
        with self._pending_lock:
            return self._pending.pop(object_id, None)
    
    def get_all_pending(self) -> List[Detection]:
        with self._pending_lock:
            return list(self._pending.values())
    
    def get_pending_ids_to_send(self, check_fn) -> List[int]:
        with self._pending_lock:
            return [d.object_id for d in self._pending.values() if check_fn(d)]
    
    def get_sent(self, object_id: int) -> Optional[DetectionRecord]:
        with self._sent_lock:
            return self._sent.get(object_id)
    
    def record_sent(self, detection: Detection):
        with self._sent_lock:
            existing = self._sent.get(detection.object_id)
            send_count = (existing.send_count + 1) if existing else 1
            current_time = time.time()
            self._sent[detection.object_id] = DetectionRecord(
                object_id=detection.object_id,
                quality_score=detection.quality_score,
                sent_timestamp=current_time,
                send_count=send_count,
                last_seen_timestamp=current_time
            )
    
    def update_last_seen(self, object_id: int):
        """Update last seen timestamp for an object"""
        with self._sent_lock:
            if object_id in self._sent:
                self._sent[object_id].last_seen_timestamp = time.time()
    
    def cleanup_stale_records(self) -> Tuple[int, int]:
        """Remove stale records that haven't been seen for TTL duration.
        Returns: (removed_sent_count, removed_pending_count)
        """
        current_time = time.time()
        removed_sent = 0
        removed_pending = 0
        
        # Cleanup sent records
        with self._sent_lock:
            stale_ids = [
                oid for oid, record in self._sent.items()
                if (current_time - record.last_seen_timestamp) > self.sent_record_ttl_sec
            ]
            for oid in stale_ids:
                del self._sent[oid]
                removed_sent += 1
        
        # Cleanup pending detections
        with self._pending_lock:
            stale_pending_ids = [
                oid for oid, detection in self._pending.items()
                if (current_time - detection.timestamp) > self.pending_ttl_sec
            ]
            for oid in stale_pending_ids:
                del self._pending[oid]
                removed_pending += 1
        
        return removed_sent, removed_pending
    
    def get_store_size(self) -> Tuple[int, int]:
        """Get current size of stores. Returns: (pending_count, sent_count)"""
        with self._pending_lock:
            pending_count = len(self._pending)
        with self._sent_lock:
            sent_count = len(self._sent)
        return pending_count, sent_count
    
    def clear(self):
        with self._pending_lock:
            self._pending.clear()
        with self._sent_lock:
            self._sent.clear()


class DetectionManager:
    """Main manager class for handling face detections."""
    
    def __init__(self, sender: Optional[KafkaSender] = None, 
                 strategy: Optional[SendStrategy] = None,
                 enabled: bool = True,
                 sent_record_ttl_sec: float = 60.0,
                 pending_ttl_sec: float = 10.0,
                 cleanup_interval_sec: float = 30.0):
        self.sender = sender
        self.strategy = strategy or DelayedBestQualitySendStrategy()
        self.store = DetectionStore(sent_record_ttl_sec, pending_ttl_sec)
        self.enabled = enabled
        self.cleanup_interval_sec = cleanup_interval_sec
        self._last_cleanup_time = time.time()
        self._stats = {'queued': 0, 'sent': 0, 'skipped': 0, 'failed': 0, 'resent': 0, 'cleaned_sent': 0, 'cleaned_pending': 0}
        self._stats_lock = Lock()
    
    def configure(self, broker: str, topic: str, strategy: SendStrategy):
        self.sender = KafkaSender(broker, topic)
        self.strategy = strategy
        self.enabled = True
    
    def initialize(self) -> bool:
        if not self.enabled or self.sender is None:
            return False
        return self.sender.connect()
    
    def shutdown(self):
        if self.sender is not None:
            self.sender.close()
    
    def queue_detection(self, detection_data: Dict[str, Any], 
                        quality_score: float,
                        object_id: int) -> SendResult:
        if not self.enabled:
            return SendResult.SKIPPED
        
        current_time = time.time()
        sent_record = self.store.get_sent(object_id)
        is_resend = sent_record is not None
        
        # Update last seen time if we have a sent record
        if sent_record is not None:
            self.store.update_last_seen(object_id)
        
        detection = Detection(
            object_id=object_id,
            detection_data=detection_data,
            quality_score=quality_score,
            timestamp=current_time,
            is_resend=is_resend
        )
        
        pending = self.store.get_pending(object_id)
        should_queue, reason = self.strategy.should_queue(detection, pending, sent_record)
        
        if not should_queue:
            self._increment_stat('skipped')
            return SendResult.SKIPPED
        
        processed = self.strategy.on_new_detection(detection, pending)
        if processed is not None:
            self.store.set_pending(processed)
            action = "Updated" if pending else "Queued"
            sys.stdout.write(f"DEBUG - {action} detection (ID: {object_id}, quality: {quality_score:.3f})\n")
            self._increment_stat('queued')
            return SendResult.PENDING
        
        return SendResult.SKIPPED
    
    def process_pending(self) -> int:
        if not self.enabled or self.sender is None:
            return 0
        
        current_time = time.time()
        
        # Periodic cleanup
        if (current_time - self._last_cleanup_time) >= self.cleanup_interval_sec:
            self._perform_cleanup()
            self._last_cleanup_time = current_time
        
        def is_ready(d: Detection) -> bool:
            ready, _ = self.strategy.should_send(d, current_time)
            return ready
        
        ready_ids = self.store.get_pending_ids_to_send(is_ready)
        sent_count = 0
        
        for object_id in ready_ids:
            detection = self.store.remove_pending(object_id)
            if detection is None:
                continue
            
            # Re-check if this detection should still be sent
            # (in case a better one was already sent while this was pending)
            sent_record = self.store.get_sent(object_id)
            if sent_record is not None:
                improvement = detection.quality_score - sent_record.quality_score
                if improvement < self.strategy.quality_improvement_threshold:
                    sys.stdout.write(
                        f"DEBUG - Skipping pending detection (ID: {object_id}, "
                        f"quality: {detection.quality_score:.3f}) - already sent better "
                        f"({sent_record.quality_score:.3f})\n"
                    )
                    self._increment_stat('skipped')
                    continue
            
            result = self._send_detection(detection)
            if result == SendResult.SUCCESS:
                sent_count += 1
        
        return sent_count
    
    def _send_detection(self, detection: Detection) -> SendResult:
        result = self.sender.send(detection.to_dict())
        
        if result == SendResult.SUCCESS:
            self.store.record_sent(detection)
            
            quality_data = detection.detection_data.get("face_quality", {})
            quality_status = "GOOD" if quality_data.get("is_good_face") else "POOR"
            resend_str = " (RESEND)" if detection.is_resend else ""
            
            sys.stdout.write(
                f"DEBUG - Sent {quality_status} face detection{resend_str} "
                f"(ID: {detection.object_id}, quality: {detection.quality_score:.3f}, "
                f"frame: {detection.detection_data.get('frame_number', 'N/A')}, "
                f"landmarks: {quality_data.get('visible_landmarks', 0)}/"
                f"{quality_data.get('total_landmarks', 0)}) "
                f"to Kafka {self.sender.topic}\n"
            )
            
            self._increment_stat('resent' if detection.is_resend else 'sent')
        else:
            self._increment_stat('failed')
        
        return result
    
    def _perform_cleanup(self):
        """Perform cleanup of stale records"""
        removed_sent, removed_pending = self.store.cleanup_stale_records()
        
        if removed_sent > 0 or removed_pending > 0:
            with self._stats_lock:
                self._stats['cleaned_sent'] += removed_sent
                self._stats['cleaned_pending'] += removed_pending
            
            pending_count, sent_count = self.store.get_store_size()
            sys.stdout.write(
                f"DEBUG - Cleanup: removed {removed_sent} sent records, "
                f"{removed_pending} pending. Store size: {pending_count} pending, {sent_count} sent\n"
            )
    
    def _increment_stat(self, stat: str):
        with self._stats_lock:
            self._stats[stat] = self._stats.get(stat, 0) + 1
    
    def get_stats(self) -> Dict[str, int]:
        with self._stats_lock:
            return self._stats.copy()
    
    def process_pending_callback(self) -> bool:
        self.process_pending()
        return True


# =============================================================================
# DeepStream Application
# =============================================================================

MAX_ELEMENTS_IN_DISPLAY_META = 16

SOURCE = ""
INFER_CONFIG = ""
STREAMMUX_BATCH_SIZE = 1
STREAMMUX_WIDTH = 1920
STREAMMUX_HEIGHT = 1080
GPU_ID = 0

PERF_MEASUREMENT_INTERVAL_SEC = 5
JETSON = False

KAFKA_BROKER = ""
KAFKA_TOPIC = "face-detections"
KAFKA_ENABLED = False
KAFKA_SEND_DELAY_SEC = 2.0
KAFKA_QUALITY_IMPROVEMENT_THRESHOLD = 0.1
KAFKA_SENT_RECORD_TTL_SEC = 60.0  # How long to keep sent records
KAFKA_PENDING_TTL_SEC = 10.0  # How long to keep pending detections
KAFKA_CLEANUP_INTERVAL_SEC = 30.0  # How often to run cleanup

# Face quality thresholds
MIN_LANDMARK_CONFIDENCE = 0.5
MIN_VISIBLE_LANDMARKS = 3
FACE_QUALITY_THRESHOLD = 0.6
MAX_HEAD_ROTATION_ANGLE = 25.0
MIN_FRONTAL_SCORE = 0.7

perf_struct = {}

# Detection manager instance
detection_manager: DetectionManager = None


class GETFPS:
    def __init__(self, stream_id):
        self.stream_id = stream_id
        self.start_time = time.time()
        self.is_first = True
        self.frame_count = 0
        self.total_fps_time = 0
        self.total_frame_count = 0
        self.fps_lock = Lock()

    def update_fps(self):
        with self.fps_lock:
            if self.is_first:
                self.start_time = time.time()
                self.is_first = False
                self.frame_count = 0
                self.total_fps_time = 0
                self.total_frame_count = 0
            else:
                self.frame_count = self.frame_count + 1

    def get_fps(self):
        with self.fps_lock:
            end_time = time.time()
            current_time = end_time - self.start_time
            self.total_fps_time = self.total_fps_time + current_time
            self.total_frame_count = self.total_frame_count + self.frame_count
            current_fps = float(self.frame_count) / current_time
            avg_fps = float(self.total_frame_count) / self.total_fps_time
            self.start_time = end_time
            self.frame_count = 0
        return current_fps, avg_fps

    def perf_print_callback(self):
        if not self.is_first:
            current_fps, avg_fps = self.get_fps()
            sys.stdout.write(f"DEBUG - Stream {self.stream_id + 1} - FPS: {current_fps:.2f} ({avg_fps:.2f})\n")
        return True


def set_custom_bbox(obj_meta):
    border_width = 6
    font_size = 18

    x_offset = obj_meta.rect_params.left - border_width * 0.5
    y_offset = obj_meta.rect_params.top - font_size * 2 + border_width * 0.5 + 1

    # Set display text to show object ID
    obj_meta.text_params.display_text = f"ID: {obj_meta.object_id}"

    obj_meta.rect_params.border_width = border_width
    obj_meta.rect_params.border_color.red = 0.0
    obj_meta.rect_params.border_color.green = 0.0
    obj_meta.rect_params.border_color.blue = 1.0
    obj_meta.rect_params.border_color.alpha = 1.0
    obj_meta.text_params.font_params.font_name = "Ubuntu"
    obj_meta.text_params.font_params.font_size = font_size
    obj_meta.text_params.x_offset = int(min(STREAMMUX_WIDTH - 1, max(0, x_offset)))
    obj_meta.text_params.y_offset = int(min(STREAMMUX_HEIGHT - 1, max(0, y_offset)))
    obj_meta.text_params.font_params.font_color.red = 1.0
    obj_meta.text_params.font_params.font_color.green = 1.0
    obj_meta.text_params.font_params.font_color.blue = 1.0
    obj_meta.text_params.font_params.font_color.alpha = 1.0
    obj_meta.text_params.set_bg_clr = 1
    obj_meta.text_params.text_bg_clr.red = 0.0
    obj_meta.text_params.text_bg_clr.green = 0.0
    obj_meta.text_params.text_bg_clr.blue = 1.0
    obj_meta.text_params.text_bg_clr.alpha = 1.0


def assess_face_quality(landmarks, bbox=None):
    """
    Assess face quality based on landmarks and bounding box area, focusing on frontal face detection.
    Allows head tilt but detects face rotation (yaw).
    Returns: (is_good_face, quality_score, quality_metrics)
    """
    if not landmarks or len(landmarks) < 5:
        return False, 0.0, {}

    # Count visible landmarks
    visible_count = sum(1 for lm in landmarks if lm['confidence'] >= MIN_LANDMARK_CONFIDENCE)

    if visible_count < MIN_VISIBLE_LANDMARKS:
        return False, 0.0, {
            'visible_landmarks': visible_count,
            'total_landmarks': len(landmarks),
            'avg_confidence': 0.0,
            'is_frontal': False
        }

    # Calculate average confidence
    visible_confidences = [lm['confidence'] for lm in landmarks if lm['confidence'] >= MIN_LANDMARK_CONFIDENCE]
    avg_confidence = sum(visible_confidences) / len(visible_confidences)

    # Extract key landmarks (5-point: left_eye, right_eye, nose, left_mouth, right_mouth)
    left_eye = landmarks[0]
    right_eye = landmarks[1]
    nose = landmarks[2]

    is_frontal = False
    frontal_score = 0.0

    # Check if key landmarks are visible
    if (left_eye['confidence'] >= MIN_LANDMARK_CONFIDENCE and 
        right_eye['confidence'] >= MIN_LANDMARK_CONFIDENCE and
        nose['confidence'] >= MIN_LANDMARK_CONFIDENCE):

        # Calculate eye distance (baseline)
        eye_distance = math.sqrt((right_eye['x'] - left_eye['x'])**2 + (right_eye['y'] - left_eye['y'])**2)

        if eye_distance > 0:
            # Calculate eyes center
            eyes_center_x = (left_eye['x'] + right_eye['x']) / 2.0

            # Check if nose is centered between eyes (frontal face indicator)
            # Nose should be within 30% of eye distance from center
            nose_offset = abs(nose['x'] - eyes_center_x)
            max_offset = eye_distance * 0.3

            # Calculate frontal score based on nose position
            if nose_offset <= max_offset:
                frontal_score = 1.0 - (nose_offset / max_offset)
            else:
                frontal_score = 0.0

            # Determine if face is frontal
            is_frontal = frontal_score >= MIN_FRONTAL_SCORE

    # --- Add box area penalty ---
    box_area_score = 1.0
    min_box_area = 50 * 50  # Minimum area for a good face (configurable)
    max_box_area = STREAMMUX_WIDTH * STREAMMUX_HEIGHT
    if bbox is not None:
        width = bbox.get('width', 0)
        height = bbox.get('height', 0)
        area = width * height
        # Penalize if area is too small
        if area < min_box_area:
            box_area_score = max(0.0, area / min_box_area)
        else:
            # Optionally, penalize if area is too large (very close face)
            box_area_score = min(1.0, area / (max_box_area * 0.4))
            box_area_score = max(box_area_score, 0.5)  # Don't penalize too much for large faces

    # Calculate overall quality score
    landmarks_weighted = 0.2  # Weight for landmarks confidence
    frontal_weighted = 0.5    # Weight for frontal detection
    box_area_weighted = 0.3   # Weight for box area

    quality_score = (
        avg_confidence * landmarks_weighted +
        frontal_score * frontal_weighted +
        box_area_score * box_area_weighted
    )

    quality_metrics = {
        'is_frontal': is_frontal,
        'visible_landmarks': visible_count,
        'total_landmarks': len(landmarks),
        'avg_confidence': round(avg_confidence, 3),
        'quality_score': round(quality_score, 3),
        'frontal_score': round(frontal_score, 3),
        'box_area_score': round(box_area_score, 3),
    }

    # Good face criteria: sufficient landmarks, good quality, and frontal
    is_good_face = (
        visible_count >= MIN_VISIBLE_LANDMARKS and
        quality_score >= FACE_QUALITY_THRESHOLD and
        is_frontal and
        box_area_score >= 0.5
    )

    return is_good_face, quality_score, quality_metrics


def crop_face_from_frame(frame_image, bbox, padding_ratio=0.2):
    """
    Crop face region from frame with optional padding.
    
    Args:
        frame_image: numpy array of the frame (H, W, C)
        bbox: dict with left, top, width, height
        padding_ratio: ratio of padding to add around the face (default 0.2 = 20%)
    
    Returns:
        tuple: (face_image_base64, crop_bbox) where crop_bbox contains actual crop coordinates with padding
               or (None, None) if failed
    """
    if frame_image is None:
        return None, None
    
    try:
        h, w = frame_image.shape[:2]
        
        # Get bbox coordinates
        left = int(bbox['left'])
        top = int(bbox['top'])
        width = int(bbox['width'])
        height = int(bbox['height'])
        
        # Add padding
        pad_w = int(width * padding_ratio)
        pad_h = int(height * padding_ratio)
        
        # Calculate crop coordinates with padding, clamped to image bounds
        x1 = max(0, left - pad_w)
        y1 = max(0, top - pad_h)
        x2 = min(w, left + width + pad_w)
        y2 = min(h, top + height + pad_h)
        
        # Crop face region
        face_crop = frame_image[y1:y2, x1:x2]
        
        if face_crop.size == 0:
            return None, None
        
        # Convert to BGR if needed (DeepStream uses RGBA)
        if face_crop.shape[2] == 4:
            face_crop = cv2.cvtColor(face_crop, cv2.COLOR_RGBA2BGR)
        
        # Encode as JPEG
        encode_param = [int(cv2.IMWRITE_JPEG_QUALITY), 85]
        _, buffer = cv2.imencode('.jpg', face_crop, encode_param)
        
        # Convert to base64
        face_base64 = base64.b64encode(buffer).decode('utf-8')
        
        # Return both the image and the actual crop bbox (with padding)
        crop_bbox = {
            'left': x1,
            'top': y1,
            'width': x2 - x1,
            'height': y2 - y1
        }
        
        return face_base64, crop_bbox
    
    except Exception as e:
        sys.stderr.write(f"ERROR - Failed to crop face: {e}\n")
        return None, None


def send_detection_to_kafka(frame_meta, obj_meta, frame_image=None):
    """Queue detection for sending via DetectionManager"""
    global detection_manager

    if detection_manager is None or not detection_manager.enabled:
        return

    object_id = obj_meta.object_id

    # Extract landmarks
    landmarks = []
    num_joints = int(obj_meta.mask_params.size / (sizeof(c_float) * 3))
    gain = min(obj_meta.mask_params.width / STREAMMUX_WIDTH, obj_meta.mask_params.height / STREAMMUX_HEIGHT)
    pad_x = (obj_meta.mask_params.width - STREAMMUX_WIDTH * gain) * 0.5
    pad_y = (obj_meta.mask_params.height - STREAMMUX_HEIGHT * gain) * 0.5

    for i in range(num_joints):
        data = obj_meta.mask_params.get_mask_array()
        xc = (data[i * 3 + 0] - pad_x) / gain
        yc = (data[i * 3 + 1] - pad_y) / gain
        confidence = data[i * 3 + 2]

        landmarks.append({
            "x": float(xc),
            "y": float(yc),
            "confidence": float(confidence)
        })

    # Get bbox for cropping and quality
    bbox = {
        "left": obj_meta.rect_params.left,
        "top": obj_meta.rect_params.top,
        "width": obj_meta.rect_params.width,
        "height": obj_meta.rect_params.height
    }

    # Assess face quality (pass bbox)
    is_good_face, quality_score, quality_metrics = assess_face_quality(landmarks, bbox=bbox)

    # Crop face and encode as base64
    face_image_base64 = None
    crop_bbox = None
    frame_image_size = None
    if frame_image is not None:
        face_image_base64, crop_bbox = crop_face_from_frame(frame_image, bbox)
        frame_image_size = frame_image.shape[0:2]  # (H, W)
        frame_image_size = { 'width': frame_image_size[1], 'height': frame_image_size[0]}

    # Prepare detection data
    detection_data = {
        "timestamp": time.time(),
        "object_id": object_id,
        "class_id": obj_meta.class_id,
        "confidence": obj_meta.confidence,
        "frame_size": frame_image_size,
        "bbox": bbox,
        "crop_bbox": crop_bbox,  # Add crop bbox with padding
        "landmarks": landmarks,
        "face_quality": {
            "is_good_face": is_good_face,
            "quality_score": quality_score,
            **quality_metrics
        },
        "frame_number": frame_meta.frame_num,
        "source_id": frame_meta.source_id,
        "face_image": face_image_base64  # Base64 encoded face image
    }
    
    # Queue via detection manager
    detection_manager.queue_detection(
        detection_data=detection_data,
        quality_score=quality_score,
        object_id=object_id
    )


def parse_face_from_meta(batch_meta, frame_meta, obj_meta):
    display_meta = None

    num_joints = int(obj_meta.mask_params.size / (sizeof(c_float) * 3))

    gain = min(obj_meta.mask_params.width / STREAMMUX_WIDTH, obj_meta.mask_params.height / STREAMMUX_HEIGHT)

    pad_x = (obj_meta.mask_params.width - STREAMMUX_WIDTH * gain) * 0.5
    pad_y = (obj_meta.mask_params.height - STREAMMUX_HEIGHT * gain) * 0.5

    for i in range(num_joints):
        data = obj_meta.mask_params.get_mask_array()

        xc = (data[i * 3 + 0] - pad_x) / gain
        yc = (data[i * 3 + 1] - pad_y) / gain
        confidence = data[i * 3 + 2]

        if confidence < 0.5:
            continue

        if display_meta is None or display_meta.num_circles == MAX_ELEMENTS_IN_DISPLAY_META:
            display_meta = pyds.nvds_acquire_display_meta_from_pool(batch_meta)
            pyds.nvds_add_display_meta_to_frame(frame_meta, display_meta)

        circle_params = display_meta.circle_params[display_meta.num_circles]
        circle_params.xc = int(min(STREAMMUX_WIDTH - 1, max(0, xc)))
        circle_params.yc = int(min(STREAMMUX_HEIGHT - 1, max(0, yc)))
        circle_params.radius = 6
        circle_params.circle_color.red = 1.0
        circle_params.circle_color.green = 1.0
        circle_params.circle_color.blue = 1.0
        circle_params.circle_color.alpha = 1.0
        circle_params.has_bg_color = 1
        circle_params.bg_color.red = 0.0
        circle_params.bg_color.green = 0.0
        circle_params.bg_color.blue = 1.0
        circle_params.bg_color.alpha = 1.0
        display_meta.num_circles += 1


def get_frame_image(gst_buffer, frame_meta):
    """
    Extract frame image from GStreamer buffer as numpy array.
    
    Args:
        gst_buffer: GStreamer buffer
        frame_meta: NvDsFrameMeta
    
    Returns:
        numpy array of shape (H, W, C) or None if failed
    """
    try:
        # Get the surface from buffer
        n_frame = pyds.get_nvds_buf_surface(hash(gst_buffer), frame_meta.batch_id)
        
        if n_frame is None:
            return None
        
        # Convert to numpy array (makes a copy)
        frame_image = np.array(n_frame, copy=True, order='C')
        
        return frame_image
    
    except Exception as e:
        sys.stderr.write(f"ERROR - Failed to get frame image: {e}\n")
        return None


def nvosd_sink_pad_buffer_probe(pad, info, user_data):
    gst_buffer = info.get_buffer()
    if not gst_buffer:
        return Gst.PadProbeReturn.OK
    
    # Try alternate method to get batch metadata
    batch_meta = pyds.gst_buffer_get_nvds_batch_meta(hash(gst_buffer))
    if not batch_meta:
        return Gst.PadProbeReturn.OK
    
    l_frame = batch_meta.frame_meta_list
    while l_frame is not None:
        try:
            frame_meta = pyds.NvDsFrameMeta.cast(l_frame.data)
        except StopIteration:
            break

        # Extract frame image for face cropping (only if Kafka is enabled)
        frame_image = None
        if detection_manager is not None and detection_manager.enabled:
            try:
                frame_image = get_frame_image(gst_buffer, frame_meta)
            except Exception as e:
                sys.stderr.write(f"ERROR - Failed to extract frame image: {e}\n")
                frame_image = None

        l_obj = frame_meta.obj_meta_list
        while l_obj is not None:
            try:
                obj_meta = pyds.NvDsObjectMeta.cast(l_obj.data)
            except StopIteration:
                break

            try:
                parse_face_from_meta(batch_meta, frame_meta, obj_meta)
                set_custom_bbox(obj_meta)
                send_detection_to_kafka(frame_meta, obj_meta, frame_image)
            except Exception as e:
                sys.stderr.write(f"ERROR - Failed to process object metadata: {e}\n")

            try:
                l_obj = l_obj.next
            except StopIteration:
                break

        perf_struct[frame_meta.source_id].update_fps()

        try:
            l_frame = l_frame.next
        except StopIteration:
            break

    return Gst.PadProbeReturn.OK


def uridecodebin_child_added_callback(child_proxy, Object, name, user_data):
    if name.find("decodebin") != -1:
        Object.connect("child-added", uridecodebin_child_added_callback, user_data)
    elif name.find("nvv4l2decoder") != -1:
        Object.set_property("drop-frame-interval", 0)
        Object.set_property("num-extra-surfaces", 1)
        Object.set_property("qos", 0)
        if JETSON:
            Object.set_property("enable-max-performance", 1)
        else:
            Object.set_property("cudadec-memtype", 0)
            Object.set_property("gpu-id", GPU_ID)


def uridecodebin_pad_added_callback(decodebin, pad, user_data):
    nvstreammux_sink_pad = user_data

    caps = pad.get_current_caps()
    if not caps:
        caps = pad.query_caps()

    structure = caps.get_structure(0)
    name = structure.get_name()
    features = caps.get_features(0)

    if name.find("video") != -1:
        if features.contains("memory:NVMM"):
            if pad.link(nvstreammux_sink_pad) != Gst.PadLinkReturn.OK:
                sys.stderr.write("ERROR - Failed to link source to nvstreammux sink pad\n")
        else:
            sys.stderr.write("ERROR - decodebin did not pick NVIDIA decoder plugin\n")


def create_uridecodebin(stream_id, uri, nvstreammux):
    bin_name = f"source-bin-{stream_id:04d}"

    uridecodebin = Gst.ElementFactory.make("uridecodebin", bin_name)

    if "rtsp://" in uri:
        pyds.configure_source_for_ntp_sync(hash(uridecodebin))

    uridecodebin.set_property("uri", uri)

    pad_name = f"sink_{stream_id}"

    nvstreammux_sink_pad = nvstreammux.get_request_pad(pad_name)
    if not nvstreammux_sink_pad:
        sys.stderr.write(f"ERROR - Failed to get nvstreammux {pad_name} pad\n")
        return None

    uridecodebin.connect("pad-added", uridecodebin_pad_added_callback, nvstreammux_sink_pad)
    uridecodebin.connect("child-added", uridecodebin_child_added_callback, None)

    perf_struct[stream_id] = GETFPS(stream_id)
    GLib.timeout_add(PERF_MEASUREMENT_INTERVAL_SEC * 1000, perf_struct[stream_id].perf_print_callback)

    return uridecodebin


def bus_call(bus, message, user_data):
    loop = user_data
    t = message.type
    if t == Gst.MessageType.EOS:
        sys.stdout.write("DEBUG - EOS\n")
        loop.quit()
    elif t == Gst.MessageType.WARNING:
        error, debug = message.parse_warning()
        sys.stderr.write(f"WARNING - {error.message} - {debug}\n")
    elif t == Gst.MessageType.ERROR:
        error, debug = message.parse_error()
        sys.stderr.write(f"ERROR - {error.message} - {debug}\n")
        loop.quit()
    return True


def is_aarch64():
    return platform.uname()[4] == "aarch64"


def init_detection_manager():
    """Initialize the detection manager with configured strategy"""
    global detection_manager
    
    if not KAFKA_ENABLED:
        detection_manager = DetectionManager(enabled=False)
        return
    
    # Create sender
    sender = KafkaSender(KAFKA_BROKER, KAFKA_TOPIC)
    
    # Create strategy based on configuration
    strategy = DelayedBestQualitySendStrategy(
        delay_sec=KAFKA_SEND_DELAY_SEC,
        quality_improvement_threshold=KAFKA_QUALITY_IMPROVEMENT_THRESHOLD
    )
    
    # Create manager with TTL settings
    detection_manager = DetectionManager(
        sender=sender,
        strategy=strategy,
        enabled=True,
        sent_record_ttl_sec=KAFKA_SENT_RECORD_TTL_SEC,
        pending_ttl_sec=KAFKA_PENDING_TTL_SEC,
        cleanup_interval_sec=KAFKA_CLEANUP_INTERVAL_SEC
    )
    
    # Initialize (connect to Kafka)
    detection_manager.initialize()


def cleanup_detection_manager():
    """Cleanup detection manager"""
    global detection_manager
    
    if detection_manager is not None:
        stats = detection_manager.get_stats()
        sys.stdout.write(f"INFO - Detection stats: {stats}\n")
        detection_manager.shutdown()


def main():
    global detection_manager
    
    Gst.init(None)
    
    init_detection_manager()
    
    # Start periodic check for pending detections
    if KAFKA_ENABLED and detection_manager is not None:
        GLib.timeout_add(500, detection_manager.process_pending_callback)

    loop = GLib.MainLoop()
    
    pipeline = Gst.Pipeline.new("pipeline")
    if not pipeline:
        sys.stderr.write("ERROR - Failed to create pipeline\n")
        return -1

    nvstreammux = Gst.ElementFactory.make("nvstreammux", "nvstreammux")
    if not nvstreammux:
        sys.stderr.write("ERROR - Failed to create nvstreammux\n")
        return -1
    print("Created nvstreammux")

    if pipeline.add(nvstreammux):
        sys.stderr.write("ERROR - Failed to add nvstreammux to pipeline\n")
        return -1
    
    uridecodebin = create_uridecodebin(0, SOURCE, nvstreammux)
    if not uridecodebin or pipeline.add(uridecodebin):
        sys.stderr.write("ERROR - Failed to create uridecodebin\n")
        return -1

    nvinfer = Gst.ElementFactory.make("nvinfer", "nvinfer")
    if not nvinfer or pipeline.add(nvinfer):
        sys.stderr.write("ERROR - Failed to create nvinfer\n")
        return -1

    nvtracker = Gst.ElementFactory.make("nvtracker", "nvtracker")
    if not nvtracker or pipeline.add(nvtracker):
        sys.stderr.write("ERROR - Failed to create nvtracker\n")
        return -1

    nvvidconv = Gst.ElementFactory.make("nvvideoconvert", "nvvidconv")
    if not nvvidconv or pipeline.add(nvvidconv):
        sys.stderr.write("ERROR - Failed to create nvvidconv\n")
        return -1

    capsfilter = Gst.ElementFactory.make("capsfilter", "capsfilter")
    if not capsfilter or pipeline.add(capsfilter):
        sys.stderr.write("ERROR - Failed to create capsfilter\n")
        return -1

    nvosd = Gst.ElementFactory.make("nvdsosd", "nvdsosd")
    if not nvosd or pipeline.add(nvosd):
        sys.stderr.write("ERROR - Failed to create nvdsosd\n")
        return -1

    nvsink = None
    if JETSON:
        nvsink = Gst.ElementFactory.make("nv3dsink", "nv3dsink")
        if not nvsink or pipeline.add(nvsink):
            sys.stderr.write("ERROR - Failed to create nv3dsink\n")
            return -1
    else:
        nvsink = Gst.ElementFactory.make("nveglglessink", "nveglglessink")
        if not nvsink or pipeline.add(nvsink):
            sys.stderr.write("ERROR - Failed to create nveglglessink\n")
            return -1

    sys.stdout.write("\n")
    sys.stdout.write(f"SOURCE: {SOURCE}\n")
    sys.stdout.write(f"INFER_CONFIG: {INFER_CONFIG}\n")
    sys.stdout.write(f"STREAMMUX_BATCH_SIZE: {STREAMMUX_BATCH_SIZE}\n")
    sys.stdout.write(f"STREAMMUX_WIDTH: {STREAMMUX_WIDTH}\n")
    sys.stdout.write(f"STREAMMUX_HEIGHT: {STREAMMUX_HEIGHT}\n")
    sys.stdout.write(f"GPU_ID: {GPU_ID}\n")
    sys.stdout.write(f"PERF_MEASUREMENT_INTERVAL_SEC: {PERF_MEASUREMENT_INTERVAL_SEC}\n")
    sys.stdout.write(f"JETSON: {'TRUE' if JETSON else 'FALSE'}\n")
    if KAFKA_ENABLED:
        sys.stdout.write(f"KAFKA_BROKER: {KAFKA_BROKER}\n")
        sys.stdout.write(f"KAFKA_TOPIC: {KAFKA_TOPIC}\n")
        sys.stdout.write(f"KAFKA_SEND_DELAY_SEC: {KAFKA_SEND_DELAY_SEC}\n")
    sys.stdout.write("\n")

    nvstreammux.set_property("batch-size", STREAMMUX_BATCH_SIZE)
    nvstreammux.set_property("batched-push-timeout", 25000)
    nvstreammux.set_property("width", STREAMMUX_WIDTH)
    nvstreammux.set_property("height", STREAMMUX_HEIGHT)
    nvstreammux.set_property("live-source", 1)
    # Add buffer pool size to reduce memory pressure
    nvstreammux.set_property("buffer-pool-size", 4)
    
    nvinfer.set_property("config-file-path", INFER_CONFIG)
    nvinfer.set_property("qos", 0)
    # Reduce batch size for inference to reduce memory usage
    nvinfer.set_property("batch-size", 1)
    
    nvtracker.set_property("tracker-width", 640)
    nvtracker.set_property("tracker-height", 384)
    nvtracker.set_property("ll-lib-file", "/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so")
    nvtracker.set_property("ll-config-file", "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml")
    nvtracker.set_property("gpu-id", GPU_ID)
    nvtracker.set_property("display-tracking-id", 1)
    nvosd.set_property("process-mode", 1)
    nvosd.set_property("qos", 0)
    nvsink.set_property("async", 0)
    nvsink.set_property("sync", 0)
    nvsink.set_property("qos", 0)
    nvsink.set_property("window-width", 400)
    nvsink.set_property("window-height", 300)

    # Set capsfilter to force RGBA format (required for face cropping)
    caps = Gst.Caps.from_string("video/x-raw(memory:NVMM), format=RGBA")
    capsfilter.set_property("caps", caps)

    if SOURCE.startswith("file://"):
        nvstreammux.set_property("live-source", 0)

    if not JETSON:
        nvstreammux.set_property("nvbuf-memory-type", 1)
        nvstreammux.set_property("gpu_id", GPU_ID)
        nvinfer.set_property("gpu_id", GPU_ID)
        nvvidconv.set_property("nvbuf-memory-type", 1)
        nvvidconv.set_property("gpu_id", GPU_ID)
        nvosd.set_property("gpu_id", GPU_ID)

    nvstreammux.link(nvinfer)
    nvinfer.link(nvtracker)
    nvtracker.link(nvvidconv)
    nvvidconv.link(capsfilter)
    capsfilter.link(nvosd)
    nvosd.link(nvsink)

    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", bus_call, loop)

    nvosd_sink_pad = nvosd.get_static_pad("sink")
    if not nvosd_sink_pad:
        sys.stderr.write("ERROR - Failed to get nvosd sink pad\n")
        return -1

    nvosd_sink_pad.add_probe(Gst.PadProbeType.BUFFER, nvosd_sink_pad_buffer_probe, None)

    pipeline.set_state(Gst.State.PAUSED)

    if pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
        sys.stderr.write("ERROR - Failed to set pipeline to playing\n")
        return -1

    sys.stdout.write("\n")

    try:
        loop.run()
    except KeyboardInterrupt:
        sys.stdout.write("\nINFO - Interrupted by user\n")
    except Exception as e:
        sys.stderr.write(f"ERROR - Unexpected error in main loop: {e}\n")

    pipeline.set_state(Gst.State.NULL)
    
    cleanup_detection_manager()

    sys.stdout.write("\n")

    return 0


def parse_args():
    global SOURCE, INFER_CONFIG, STREAMMUX_BATCH_SIZE, STREAMMUX_WIDTH, STREAMMUX_HEIGHT, GPU_ID, JETSON
    global KAFKA_BROKER, KAFKA_TOPIC, KAFKA_ENABLED, KAFKA_SEND_DELAY_SEC, KAFKA_QUALITY_IMPROVEMENT_THRESHOLD

    parser = argparse.ArgumentParser(description="DeepStream")
    parser.add_argument("-s", "--source", required=True, help="Source stream/file")
    parser.add_argument("-c", "--infer-config", required=True, help="Config infer file")
    parser.add_argument("-b", "--streammux-batch-size", type=int, default=1, help="Streammux batch-size (default 1)")
    parser.add_argument("-w", "--streammux-width", type=int, default=1920, help="Streammux width (default 1920)")
    parser.add_argument("-e", "--streammux-height", type=int, default=1080, help="Streammux height (default 1080)")
    parser.add_argument("-g", "--gpu-id", type=int, default=0, help="GPU id (default 0)")
    parser.add_argument("--kafka-broker", help="Kafka broker address (e.g., localhost:9092)")
    parser.add_argument("--kafka-topic", default="face-detections", help="Kafka topic name (default: face-detections)")
    parser.add_argument("--kafka-delay", type=float, default=2.0, help="Delay in seconds before sending to Kafka (default: 2.0)")
    parser.add_argument("--kafka-quality-threshold", type=float, default=0.005, help="Minimum quality improvement to resend (default: 0.005)")
    args = parser.parse_args()

    if args.source == "":
        sys.stderr.write("ERROR - Source not found\n")
        sys.exit(-1)

    if args.infer_config == "" or not os.path.isfile(args.infer_config):
        sys.stderr.write("ERROR - Config infer not found\n")
        sys.exit(-1)

    SOURCE = args.source
    INFER_CONFIG = args.infer_config
    STREAMMUX_BATCH_SIZE = args.streammux_batch_size
    STREAMMUX_WIDTH = args.streammux_width
    STREAMMUX_HEIGHT = args.streammux_height
    GPU_ID = args.gpu_id
    
    if args.kafka_broker:
        KAFKA_BROKER = args.kafka_broker
        KAFKA_TOPIC = args.kafka_topic
        KAFKA_ENABLED = True
        KAFKA_SEND_DELAY_SEC = args.kafka_delay
        KAFKA_QUALITY_IMPROVEMENT_THRESHOLD = args.kafka_quality_threshold

    JETSON = is_aarch64()


if __name__ == "__main__":
    parse_args()
    sys.exit(main())
