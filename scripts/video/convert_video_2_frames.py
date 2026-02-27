import sys
import time
import os
import math
import cv2

# Convert video file to frame images and save to disk
# wsl, .\venv\Scripts\activate
# Usage: python3 convert_video_2_frames.py [video_path]


DEFAULT_VIDEO_PATH = "../../videos/sample.mp4"
OUTPUT_FRAME_DIR = "../../outputs/sample_frames"
DEFAULT_SKIP_FRAME_INTERVAL = 1  # Save every Nth frame (1=save all, 2=save every other frame, etc.)


def main():
	# Parse skip frame interval and video path from command line or use default
	if len(sys.argv) > 1:
		try:
			skip_frame_interval = int(sys.argv[1])
			if skip_frame_interval < 1:
				raise ValueError
		except ValueError:
			print("Error: skip_frame_interval must be a positive integer.")
			sys.exit(1)
	else:
		skip_frame_interval = DEFAULT_SKIP_FRAME_INTERVAL

	if len(sys.argv) > 2:
		video_path = sys.argv[2]
	else:
		video_path = DEFAULT_VIDEO_PATH


	# Output directory
	output_dir = OUTPUT_FRAME_DIR
	os.makedirs(output_dir, exist_ok=True)

	# Clear images in output directory
	for fname in os.listdir(output_dir):
		if fname.lower().endswith(('.jpg', '.jpeg')):
			try:
				os.remove(os.path.join(output_dir, fname))
			except Exception as e:
				print(f"Warning: Could not remove {fname}: {e}")

	# Open video file
	cap = cv2.VideoCapture(video_path)
	if not cap.isOpened():
		print(f"Error: Cannot open video file {video_path}")
		sys.exit(1)

	total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
	fps = cap.get(cv2.CAP_PROP_FPS)
	print(f"Video: {video_path}")
	print(f"Total frames: {total_frames}, FPS: {fps}")
	print(f"Skip frame interval: {skip_frame_interval}")

	frame_idx = 0
	saved_idx = 0
	start_time = time.time()
	while True:
		ret, frame = cap.read()
		if not ret:
			break
		if frame_idx % skip_frame_interval == 0:
			# Draw frame number on the frame
			text = f"Frame: {frame_idx}"
			font = cv2.FONT_HERSHEY_SIMPLEX
			font_scale = 1.0
			color = (0, 255, 0)  # Green
			thickness = 2
			org = (30, 50)
			cv2.putText(frame, text, org, font, font_scale, color, thickness, cv2.LINE_AA)

			frame_filename = os.path.join(output_dir, f"frame_{frame_idx:06d}.jpg")
			cv2.imwrite(frame_filename, frame)
			if saved_idx % 100 == 0:
				print(f"Saved frame {frame_idx}/{total_frames}")
			saved_idx += 1
		frame_idx += 1

	cap.release()
	elapsed = time.time() - start_time
	print(f"Done. {saved_idx}/{total_frames} frames saved to {output_dir} in {elapsed:.2f} seconds.")

if __name__ == "__main__":
	main()