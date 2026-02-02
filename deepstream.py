import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib

import os
import sys
import time
import argparse
import platform
import json
from threading import Lock
from ctypes import sizeof, c_float
from collections import deque
from kafka import KafkaProducer
from kafka.errors import KafkaError
import math

sys.path.append("/opt/nvidia/deepstream/deepstream/lib")
import pyds

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

# Face quality thresholds
MIN_LANDMARK_CONFIDENCE = 0.5  # Minimum confidence for a landmark to be valid
MIN_VISIBLE_LANDMARKS = 3      # Minimum number of visible landmarks for a "good" face
FACE_QUALITY_THRESHOLD = 0.6   # Minimum quality score (0-1) to send to Kafka
MAX_HEAD_ROTATION_ANGLE = 25.0 # Maximum head rotation angle (degrees) for frontal face
MIN_FRONTAL_SCORE = 0.7        # Minimum frontal score (0-1) for frontal face

perf_struct = {}
kafka_producer = None
seen_object_ids = deque(maxlen=100)


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


def assess_face_quality(landmarks):
    """
    Assess face quality based on landmarks, focusing on frontal face detection.
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
    
    # Calculate overall quality score
    quality_score = (
        avg_confidence * 0.5 +      # 50% weight on landmark confidence
        frontal_score * 0.5          # 50% weight on frontal detection
    )
    
    quality_metrics = {
        'is_frontal': is_frontal,
        'visible_landmarks': visible_count,
        'total_landmarks': len(landmarks),
        'avg_confidence': round(avg_confidence, 3),
        'quality_score': round(quality_score, 3),
        'frontal_score': round(frontal_score, 3),
    }
    
    # Good face criteria: sufficient landmarks, good quality, and frontal
    is_good_face = (
        visible_count >= MIN_VISIBLE_LANDMARKS and
        quality_score >= FACE_QUALITY_THRESHOLD and
        is_frontal
    )
    
    return is_good_face, quality_score, quality_metrics


def send_detection_to_kafka(frame_meta, obj_meta):
    """Send detection information to Kafka for new object IDs"""
    global kafka_producer, seen_object_ids
    
    if not KAFKA_ENABLED or kafka_producer is None:
        return
    
    object_id = obj_meta.object_id
    
    # Only send if this is a new object ID
    if object_id in seen_object_ids:
        return
    
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
    
    # Assess face quality
    is_good_face, quality_score, quality_metrics = assess_face_quality(landmarks)
    
    # Optionally, only send good faces to Kafka
    # Uncomment the following lines to filter out low-quality faces:
    # if not is_good_face:
    #     sys.stdout.write(f"DEBUG - Skipping low-quality face (ID: {object_id}, quality: {quality_score:.3f})\n")
    #     return
    
    # Mark as seen regardless of quality
    seen_object_ids.append(object_id)
    
    # Prepare detection data
    detection_data = {
        "timestamp": time.time(),
        "object_id": object_id,
        "class_id": obj_meta.class_id,
        "confidence": obj_meta.confidence,
        "bbox": {
            "left": obj_meta.rect_params.left,
            "top": obj_meta.rect_params.top,
            "width": obj_meta.rect_params.width,
            "height": obj_meta.rect_params.height
        },
        "landmarks": landmarks,
        "face_quality": {
            "is_good_face": is_good_face,
            "quality_score": quality_score,
            **quality_metrics
        },
        "frame_number": frame_meta.frame_num,
        "source_id": frame_meta.source_id
    }
    
    try:
        # Send to Kafka and get future
        future = kafka_producer.send(KAFKA_TOPIC, value=detection_data)
        # Optionally wait for confirmation (with timeout)
        record_metadata = future.get(timeout=1)
        quality_status = "GOOD" if is_good_face else "POOR"
        sys.stdout.write(f"DEBUG - Sent {quality_status} face detection (ID: {object_id}, quality: {quality_score:.3f}, frame: {frame_meta.frame_num}, landmarks: {quality_metrics['visible_landmarks']}/{quality_metrics['total_landmarks']}) to Kafka {KAFKA_TOPIC}\n")
    except KafkaError as e:
        sys.stderr.write(f"ERROR - Failed to send to Kafka: {e}\n")
    except Exception as e:
        sys.stderr.write(f"ERROR - Unexpected error sending to Kafka: {e}\n")


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

        l_obj = frame_meta.obj_meta_list
        while l_obj is not None:
            try:
                obj_meta = pyds.NvDsObjectMeta.cast(l_obj.data)
            except StopIteration:
                break

            parse_face_from_meta(batch_meta, frame_meta, obj_meta)
            set_custom_bbox(obj_meta)
            send_detection_to_kafka(frame_meta, obj_meta)

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
        pyds.configure_source_for_ntp_sync(uridecodebin)

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


def init_kafka_producer():
    """Initialize Kafka producer"""
    global kafka_producer
    
    if not KAFKA_ENABLED:
        return
    
    try:
        kafka_producer = KafkaProducer(
            bootstrap_servers=KAFKA_BROKER,
            value_serializer=lambda v: json.dumps(v).encode('utf-8'),
            acks=1,  # Wait for leader acknowledgment
            compression_type='gzip',
            linger_ms=10,  # Batch messages for 10ms
            request_timeout_ms=30000,
            retries=3
        )
        # Test connection by getting metadata
        kafka_producer.bootstrap_connected()
        sys.stdout.write(f"INFO - Kafka producer initialized (broker: {KAFKA_BROKER}, topic: {KAFKA_TOPIC})\n")
    except Exception as e:
        sys.stderr.write(f"ERROR - Failed to initialize Kafka producer: {e}\n")
        kafka_producer = None


def cleanup_kafka_producer():
    """Cleanup Kafka producer"""
    global kafka_producer
    
    if kafka_producer is not None:
        try:
            kafka_producer.flush()
            kafka_producer.close()
            sys.stdout.write("INFO - Kafka producer closed\n")
        except Exception as e:
            sys.stderr.write(f"ERROR - Failed to close Kafka producer: {e}\n")


def main():
    Gst.init(None)
    
    init_kafka_producer()

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
    sys.stdout.write("\n")

    nvstreammux.set_property("batch-size", STREAMMUX_BATCH_SIZE)
    nvstreammux.set_property("batched-push-timeout", 25000)
    nvstreammux.set_property("width", STREAMMUX_WIDTH)
    nvstreammux.set_property("height", STREAMMUX_HEIGHT)
    nvstreammux.set_property("live-source", 1)
    nvinfer.set_property("config-file-path", INFER_CONFIG)
    nvinfer.set_property("qos", 0)
    nvtracker.set_property("tracker-width", 640)
    nvtracker.set_property("tracker-height", 384)
    nvtracker.set_property("ll-lib-file", "/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so")
    nvtracker.set_property("ll-config-file", "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml")
    nvtracker.set_property("gpu-id", GPU_ID)
    nvtracker.set_property("display-tracking-id", 1)
    nvosd.set_property("process-mode", 1)  # GPU process mode
    nvosd.set_property("qos", 0)
    nvsink.set_property("async", 0)
    nvsink.set_property("sync", 0)
    nvsink.set_property("qos", 0)

    # set width and height for nvsink view
    nvsink.set_property("window-width", 400)
    nvsink.set_property("window-height", 300)


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
    except:
        pass

    pipeline.set_state(Gst.State.NULL)
    
    cleanup_kafka_producer()

    sys.stdout.write("\n")

    return 0


def parse_args():
    global SOURCE, INFER_CONFIG, STREAMMUX_BATCH_SIZE, STREAMMUX_WIDTH, STREAMMUX_HEIGHT, GPU_ID, JETSON
    global KAFKA_BROKER, KAFKA_TOPIC, KAFKA_ENABLED

    parser = argparse.ArgumentParser(description="DeepStream")
    parser.add_argument("-s", "--source", required=True, help="Source stream/file")
    parser.add_argument("-c", "--infer-config", required=True, help="Config infer file")
    parser.add_argument("-b", "--streammux-batch-size", type=int, default=1, help="Streammux batch-size (default 1)")
    parser.add_argument("-w", "--streammux-width", type=int, default=1920, help="Streammux width (default 1920)")
    parser.add_argument("-e", "--streammux-height", type=int, default=1080, help="Streammux height (default 1080)")
    parser.add_argument("-g", "--gpu-id", type=int, default=0, help="GPU id (default 0)")
    parser.add_argument("--kafka-broker", help="Kafka broker address (e.g., localhost:9092)")
    parser.add_argument("--kafka-topic", default="face-detections", help="Kafka topic name (default: face-detections)")
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

    JETSON = is_aarch64()


if __name__ == "__main__":
    parse_args()
    sys.exit(main())
