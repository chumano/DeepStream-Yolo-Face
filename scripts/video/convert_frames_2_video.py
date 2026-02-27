import sys
import time
import os
import math
import cv2
import contextlib

# Convert frame images to video file and save to disk
# wsl, .\venv\Scripts\activate
# Usage: python3 convert_frames_2_video.py [video_path]

FRAMES_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../outputs/sample_frames'))
DEFAULT_VIDEO_FOLDER = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../outputs'))

DEFAULT_FPS = 30
DEFAULT_CODEC = 'mp4v'  # Options: 'h264', 'mp4v'
# h264 not working

def get_fourcc(codec):
	codec = codec.lower()
	if codec == 'h264' or codec == 'avc1':
		# Use 'X264' (libx264 software encoder) instead of 'avc1' (h264_v4l2m2m hardware)
		# 'avc1' / h264_v4l2m2m requires V4L2 hardware support, which is unavailable in WSL
		return cv2.VideoWriter_fourcc(*'X264')
	elif codec == 'mp4v':
		return cv2.VideoWriter_fourcc(*'mp4v')
	else:
		print(f"Unknown codec '{codec}', defaulting to 'mp4v'.")
		return cv2.VideoWriter_fourcc(*'mp4v')

@contextlib.contextmanager
def suppress_stderr():
	"""Suppress low-level C/FFmpeg stderr noise during VideoWriter init."""
	devnull_fd = os.open(os.devnull, os.O_WRONLY)
	old_stderr_fd = os.dup(2)
	try:
		os.dup2(devnull_fd, 2)
		yield
	finally:
		os.dup2(old_stderr_fd, 2)
		os.close(devnull_fd)
		os.close(old_stderr_fd)

def get_frame_files(frames_dir):
	files = [f for f in os.listdir(frames_dir) if f.lower().endswith(('.jpg', '.jpeg', '.png'))]
	files.sort()  # Assumes frames are named in order
	return [os.path.join(frames_dir, f) for f in files]

def main():
	try:
		fps = float(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_FPS
	except ValueError:
		print(f"Invalid FPS value: {sys.argv[1]}. Using default FPS={DEFAULT_FPS}.")
		fps = DEFAULT_FPS

	# Codec selection: sys.argv[3] (optional)
	codec = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_CODEC
	video_name = sys.argv[2] if len(sys.argv) > 2 else f"sample_{int(fps)}_{codec}.mp4"
	video_path = os.path.join(DEFAULT_VIDEO_FOLDER, video_name)

	print(f"Video path: {video_path}, FPS: {fps}, Codec: {codec}")

	frames_dir = FRAMES_DIR
	frame_files = get_frame_files(frames_dir)
	if not frame_files:
		print(f"No frame images found in {frames_dir}")
		sys.exit(1)

	# Show number of frames and first few frame files for debugging
	print(f"Found {len(frame_files)} frame files in {frames_dir}")
	print("First few frame files:", frame_files[:5])

	# Read first frame to get size
	first_frame = cv2.imread(frame_files[0])
	if first_frame is None:
		print(f"Failed to read first frame: {frame_files[0]}")
		sys.exit(1)
	height, width, channels = first_frame.shape

	fourcc = get_fourcc(codec)
	#with suppress_stderr():
	#	out = cv2.VideoWriter(video_path, fourcc, fps, (width, height))
	out = cv2.VideoWriter(video_path, fourcc, fps, (width, height))

	if not out.isOpened():
		print(f"Failed to open video writer with codec '{codec}'. Trying fallback to 'mp4v'.")
		fourcc = get_fourcc('mp4v')
		out = cv2.VideoWriter(video_path, fourcc, fps, (width, height))
		if not out.isOpened():
			print("Failed to open video writer with fallback codec 'mp4v'. Exiting.")
			sys.exit(1)

	for idx, frame_file in enumerate(frame_files):
		frame = cv2.imread(frame_file)
		if frame is None:
			print(f"Warning: failed to read {frame_file}, skipping.")
			continue
		if (frame.shape[1], frame.shape[0]) != (width, height):
			frame = cv2.resize(frame, (width, height))
		out.write(frame)
		if (idx+1) % 50 == 0:
			print(f"Written {idx+1}/{len(frame_files)} frames...")

	out.release()
	print(f"Video saved to {video_path} (FPS={fps}, Codec={codec})")
if __name__ == "__main__":
    print(cv2.getBuildInformation())
    main()