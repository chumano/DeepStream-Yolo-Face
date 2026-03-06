import sys
import time
import os
import math
import av # PyAV library `pip install av==16.1.0` (FFmpeg bindings for Python)


RTSP_URL = "rtsp://100.64.0.153:8554/test"
# Debug: export AV_LOG_LEVEL=debug
# Usage: python3 read_rtsp.py [rtsp_url]
def read_rtsp(rtsp_url):
    try:
        print(f"Opening RTSP stream: {rtsp_url}")   
        container = av.open(rtsp_url,
                options={
                "rtsp_transport": "tcp",
                "fflags": "nobuffer",
                "flags": "low_delay",
                "max_delay": "500000",  # 500ms
            })
        if container.duration is not None:
            duration_str = f"{container.duration / av.time_base:.2f} seconds"
        else:
            duration_str = "unknown"
        print(f"Stream opened successfully. Duration: {duration_str}, "
              f"Number of streams: {len(container.streams)}, "
              f"Video stream index: {container.streams.video[0].index}")
        video_stream = container.streams.video[0]
        frame_count = 0
        for packet in container.demux(video_stream):
            for frame in packet.decode():
                pts = frame.pts
                dts = packet.dts
                time_base = video_stream.time_base

                if pts is not None:
                    pts_seconds = float(pts * time_base)
                else:
                    pts_seconds = None

                if dts is not None:
                    dts_seconds = float(dts * time_base)
                else:
                    dts_seconds = None

                frame_count += 1
                # Frame 506: PTS=2164091 (24.045s), DTS=2164091 (24.045s), Size=1920x1080
                print(f"Frame {frame_count}: PTS={pts} ({pts_seconds:.3f}s), DTS={dts} ({dts_seconds:.3f}s), NTP={ntp}, Size={frame.width}x{frame.height}")
    except Exception as e:
        print(f"Error occurred while reading RTSP stream: {e}", file=sys.stderr)

if __name__ == "__main__":
    if len(sys.argv) > 1:
        rtsp_url = sys.argv[1]
    else:
        rtsp_url = RTSP_URL
    read_rtsp(rtsp_url)