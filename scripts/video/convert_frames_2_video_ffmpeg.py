
import subprocess
import os
import argparse
import tempfile

# Convert frame images to video file and save to disk
# wsl, .\venv\Scripts\activate
# Usage: python3 convert_frames_2_video_ffmpeg.py
# Usage: python3 convert_frames_2_video_ffmpeg.py --frames_dir /path/to/frames --output /path/to/output.mp4 --fps 30

FRAMES_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../outputs/sample_frames'))
DEFAULT_VIDEO_FOLDER = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../outputs'))

DEFAULT_FPS = 30
DEFAULT_CODEC = 'h264'


def get_frame_files(frames_dir):
    files = [f for f in os.listdir(frames_dir) if f.lower().endswith(('.jpg', '.jpeg', '.png'))]
    files.sort()  # Assumes frames are named in order
    return [os.path.join(frames_dir, f) for f in files]


def build_concat_file(frame_files, fps):
    """Write a ffmpeg concat demuxer file listing each frame with its duration."""
    duration = 1.0 / fps
    tmp = tempfile.NamedTemporaryFile(mode='w', suffix='.txt', delete=False)
    for f in frame_files:
        # ffmpeg concat demuxer requires forward slashes
        safe_path = f.replace('\\', '/')
        tmp.write(f"file '{safe_path}'\n")
        tmp.write(f"duration {duration:.6f}\n")
    # Repeat last frame to avoid a cut-off at the very end
    if frame_files:
        safe_path = frame_files[-1].replace('\\', '/')
        tmp.write(f"file '{safe_path}'\n")
    tmp.close()
    return tmp.name


def convert_frames_to_video(
    frames_dir=FRAMES_DIR,
    output_path=None,
    fps=DEFAULT_FPS,
    codec=DEFAULT_CODEC,
    crf=23,
    preset='fast',
):
    """
    Convert all image frames in frames_dir to an MP4 video using ffmpeg.

    Args:
        frames_dir  : Directory containing frame images (jpg/jpeg/png).
        output_path : Output .mp4 file path. Defaults to <outputs>/output.mp4.
        fps         : Frames per second for the output video.
        codec       : Video codec: h264, h265, vp9 …
        crf         : Constant Rate Factor – quality (0-51, lower = better).
        preset      : ffmpeg encoding preset (ultrafast … veryslow).
    """
    frames_dir = os.path.abspath(frames_dir)
    if not os.path.isdir(frames_dir):
        raise ValueError(f"Frames directory not found: {frames_dir}")

    frame_files = get_frame_files(frames_dir)
    if not frame_files:
        raise RuntimeError(f"No image frames found in: {frames_dir}")

    print(f"Found {len(frame_files)} frame(s) in '{frames_dir}'")

    if output_path is None:
        output_path = os.path.join(DEFAULT_VIDEO_FOLDER, 'output.mp4')
    output_path = os.path.abspath(output_path)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    concat_file = build_concat_file(frame_files, fps)
    print(f"Concat list written to: {concat_file}")

    # Map friendly codec names to ffmpeg encoder names
    codec_map = {
        'h264': 'libx264',
        'h265': 'libx265',
        'hevc': 'libx265',
        'vp9':  'libvpx-vp9',
    }
    encoder = codec_map.get(codec.lower(), codec)

    cmd = [
        'ffmpeg', '-y',
        '-f', 'concat',
        '-safe', '0',
        '-i', concat_file,
        '-vcodec', encoder,
        '-crf', str(crf),
        '-preset', preset,
        '-pix_fmt', 'yuv420p',
        '-r', str(fps),  # Output frame rate
        '-movflags', '+faststart',
        output_path,
    ]

    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd)

    os.unlink(concat_file)

    if result.returncode != 0:
        raise RuntimeError(f"ffmpeg failed with return code {result.returncode}")

    print(f"Video saved to: {output_path}")
    return output_path


def main():
    parser = argparse.ArgumentParser(
        description='Convert image frames in a folder to an MP4 video using ffmpeg.'
    )
    parser.add_argument(
        '--frames_dir', '-d',
        default=FRAMES_DIR,
        help=f'Directory containing frame images (default: {FRAMES_DIR})',
    )
    parser.add_argument(
        '--output', '-o',
        default=None,
        help='Output video file path (default: <outputs>/output.mp4)',
    )
    parser.add_argument(
        '--fps', '-r',
        type=float,
        default=DEFAULT_FPS,
        help=f'Frames per second (default: {DEFAULT_FPS})',
    )
    parser.add_argument(
        '--codec', '-c',
        default=DEFAULT_CODEC,
        help=f'Video codec: h264, h265, vp9 … (default: {DEFAULT_CODEC})',
    )
    parser.add_argument(
        '--crf',
        type=int,
        default=23,
        help='Constant Rate Factor – quality (0-51, lower=better, default: 23)',
    )
    parser.add_argument(
        '--preset',
        default='fast',
        choices=['ultrafast', 'superfast', 'veryfast', 'faster', 'fast',
                 'medium', 'slow', 'slower', 'veryslow'],
        help='ffmpeg encoding preset (default: fast)',
    )

    args = parser.parse_args()
    convert_frames_to_video(
        frames_dir=args.frames_dir,
        output_path=args.output,
        fps=args.fps,
        codec=args.codec,
        crf=args.crf,
        preset=args.preset,
    )


if __name__ == '__main__':
    main()
