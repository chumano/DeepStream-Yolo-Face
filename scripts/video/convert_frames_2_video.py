import sys
import time
import os
import math
import cv2

# Convert frame images to video file and save to disk
# wsl, .\venv\Scripts\activate
# Usage: python3 convert_frames_2_video.py [video_path]

FRAMES_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../outputs/sample_frames'))
DEFAULT_VIDEO_FOLDER = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../outputs'))
DEFAULT_FPS = 30

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

	video_name = sys.argv[2] if len(sys.argv) > 2 else f"sample_{int(fps)}.mp4"
	video_path = os.path.join(DEFAULT_VIDEO_FOLDER, video_name)

	print(f"Video path: {video_path}, FPS: {fps}")
    
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

	fourcc = cv2.VideoWriter_fourcc(*'mp4v')
	out = cv2.VideoWriter(video_path, fourcc, fps, (width, height))

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
	print(f"Video saved to {video_path} (FPS={fps})")

if __name__ == "__main__":
    main()