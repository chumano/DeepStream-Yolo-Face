# RTSP

```bash
# Test RTSP stream with gst-launch-1.0
GST_DEBUG=rtpsession:5 gst-launch-1.0 rtspsrc protocols=tcp add-reference-timestamp-meta=true location=rtsp://100.64.0.153:8554/test ntp-sync=true ! identity silent=false ! fakesink

GST_DEBUG=rtpsession:5 gst-launch-1.0 rtspsrc  location=rtsp://100.64.0.153:8554/test ntp-sync=true ! identity silent=false ! fakesink

GST_DEBUG=rtpsession:5 gst-launch-1.0 rtspsrc protocols=tcp location=rtsp://100.64.0.153:8554/test ntp-sync=true ! fakesink 2>&1 | grep -v "Received TWCC packet" | tee rtsp_debug.log

# check port
Netcat	Quick connectivity check	
nc -vuz 100.64.0.153 8001

# Check if MediaMTX is listening on UDP 8001
ss -ulpn | grep 8001

GST_DEBUG=rtpsession:5 python3 src/py/test_uridecodebin.py 2>&1 | grep -v "Received TWCC packet"

# test with nvurisrcbin
gst-launch-1.0 nvurisrcbin uri=rtsp://100.64.0.153:8554/test  rtsp-reconnect-interval=15 rtsp-reconnect-attempts=-1 ! fakesink

# Enable detailed debug logs for nvurisrcbin
export GST_DEBUG=3,nvurisrcbin:6 && "the command-line of your application" >1.log 2>1.log
```

