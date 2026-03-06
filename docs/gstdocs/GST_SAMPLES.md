gst-launch-1.0 videotestsrc ! timeoverlay ! x264enc ! splitmuxsink location=video_%02d.mp4 max-size-time=60000000000

gst-launch-1.0 videotestsrc ! clockoverlay time-format="%Y-%m-%d %H:%M:%S" ! x264enc ! splitmuxsink location=video_$(date +%Y%m%d_%H%M%S)_%02d.mp4 max-size-time=60000000000