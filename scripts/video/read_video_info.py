import sys
import time
import os
import math
import cv2

# Get metadata from video file and print to console
# wsl, .\venv\Scripts\activate
# Usage: python3 read_video_info.py [video_path]
# Ouput example:
	# Video path: ../../videos/sample.mp4
	# Frame count: 1443
	# FPS: 30.00
	# Resolution: 1920x1080
	# Codec: h264
	# Duration: 48.10 seconds (0m 48.10s)

DEFAULT_VIDEO_PATH = "../../videos/sample.mp4"

def get_video_info(video_path):
	if not os.path.exists(video_path):
		print(f"Error: File not found: {video_path}")
		return
	cap = cv2.VideoCapture(video_path)
	if not cap.isOpened():
		print(f"Error: Cannot open video file: {video_path}")
		return
	frame_count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
	fps = cap.get(cv2.CAP_PROP_FPS)
	width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
	height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
	fourcc = int(cap.get(cv2.CAP_PROP_FOURCC))
	codec = "".join([chr((fourcc >> 8 * i) & 0xFF) for i in range(4)])
	duration = frame_count / fps if fps > 0 else 0
	print(f"Video path: {video_path}")
	print(f"Frame count: {frame_count}")
	print(f"FPS: {fps:.2f}")
	print(f"Resolution: {width}x{height}")
	print(f"Codec: {codec}")
	print(f"Duration: {duration:.2f} seconds ({math.floor(duration // 60)}m {duration % 60:.2f}s)")
	cap.release()

if __name__ == "__main__":
	if len(sys.argv) > 1:
		video_path = sys.argv[1]
	else:
		video_path = DEFAULT_VIDEO_PATH
	get_video_info(video_path)

	# Alternative way to get all metadata using ffprobe (part of ffmpeg)
	import subprocess
	import json
	# Show GOP frame types (I, P, B) for each frame using ffprobe:
	# ffprobe -select_streams v:0 -show_frames -show_entries frame=pict_type -of csv ../../videos/sample.mp4
	result = subprocess.run(
		[
			'ffprobe',
			'-v', 'error',
			'-show_entries', 'format=duration:format=bit_rate:format=size:format=filename:format=format_name:format=format_long_name:format=start_time:format=tags',
			'-show_streams',
			'-of', 'json',
			video_path
		],
		stdout=subprocess.PIPE, stderr=subprocess.STDOUT
	)
	try:
		metadata = json.loads(result.stdout)
		print("\nffprobe metadata:")
		print(json.dumps(metadata, indent=2))
		'''
		This looks like a multiline comment, but actually it is a multi-line string.
		{
			"streams": [
				{
				"index": 0,
				"codec_name": "h264",
				"codec_long_name": "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10",
				"profile": "High",
				"codec_type": "video",
				"codec_tag_string": "avc1",
				"codec_tag": "0x31637661",
				"width": 1920,
				"height": 1080,
				"coded_width": 1920,
				"coded_height": 1080,
				"closed_captions": 0,
				"has_b_frames": 2,
				"pix_fmt": "yuv420p",
				"level": 40,
				"chroma_location": "left",
				"refs": 1,
				"is_avc": "true",
				"nal_length_size": "4",
				"r_frame_rate": "30/1",
				"avg_frame_rate": "30/1",
				"time_base": "1/15360",
				"start_pts": 0,
				"start_time": "0.000000",
				"duration_ts": 738816,
				"duration": "48.100000",
				"bit_rate": "5675961",
				"bits_per_raw_sample": "8",
				"nb_frames": "1443",
				"disposition": {
					"default": 1,
					"dub": 0,
					"original": 0,
					"comment": 0,
					"lyrics": 0,
					"karaoke": 0,
					"forced": 0,
					"hearing_impaired": 0,
					"visual_impaired": 0,
					"clean_effects": 0,
					"attached_pic": 0,
					"timed_thumbnails": 0
				},
				"tags": {
					"language": "eng",
					"handler_name": "VideoHandler",
					"vendor_id": "[0][0][0][0]"
				}
				},
				{
				"index": 1,
				"codec_name": "aac",
				"codec_long_name": "AAC (Advanced Audio Coding)",
				"profile": "LC",
				"codec_type": "audio",
				"codec_tag_string": "mp4a",
				"codec_tag": "0x6134706d",
				"sample_fmt": "fltp",
				"sample_rate": "48000",
				"channels": 2,
				"channel_layout": "stereo",
				"bits_per_sample": 0,
				"r_frame_rate": "0/0",
				"avg_frame_rate": "0/0",
				"time_base": "1/48000",
				"start_pts": 0,
				"start_time": "0.000000",
				"duration_ts": 2305024,
				"duration": "48.021333",
				"bit_rate": "128825",
				"nb_frames": "2250",
				"disposition": {
					"default": 1,
					"dub": 0,
					"original": 0,
					"comment": 0,
					"lyrics": 0,
					"karaoke": 0,
					"forced": 0,
					"hearing_impaired": 0,
					"visual_impaired": 0,
					"clean_effects": 0,
					"attached_pic": 0,
					"timed_thumbnails": 0
				},
				"tags": {
					"language": "eng",
					"handler_name": "SoundHandler",
					"vendor_id": "[0][0][0][0]"
				}
				}
			],
			"format": {
				"filename": "../../videos/sample.mp4",
				"format_name": "mov,mp4,m4a,3gp,3g2,mj2",
				"format_long_name": "QuickTime / MOV",
				"start_time": "0.000000",
				"duration": "48.100000",
				"size": "34952625",
				"bit_rate": "5813326"
			}
		}
		'''
	except Exception as e:
		print(f"Error parsing ffprobe output: {e}")
		print(f"Raw ffprobe output: {result.stdout.decode('utf-8')}")