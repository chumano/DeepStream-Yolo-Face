#!/usr/bin/env python3

import datetime

import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst, GLib

Gst.init(None)

RTSP_URL = "rtsp://100.64.0.153:8554/test"

# rtspsrc
#    → rtpbin
#       → RTPSession
#          → RTPSource

def get_ntp_from_source(source):

    stats = source.get_property("stats")

    if stats is None:
        print("No stats available")
        return

    # stats is Gst.Structure
    ntp = stats.get_value("sr-ntptime")
    rtp = stats.get_value("sr-rtptime")

    print("Raw NTP:", ntp)
    print("RTP timestamp:", rtp)

    if ntp is None:
        print("No sender report yet")
        return

    # split 64-bit NTP
    seconds = ntp >> 32
    fraction = ntp & 0xffffffff

    frac_seconds = fraction / (2**32)

    ntp_epoch = datetime.datetime(1900, 1, 1)

    dt = ntp_epoch + datetime.timedelta(seconds=seconds + frac_seconds)

    print("Datetime:", dt)

    return dt

def print_element_info(element):
    return
    print(f"Element: {element.get_name()}, Type: {element.__class__.__name__}")
    print("Properties:")
    for prop in element.list_properties():
        try:
            value = element.get_property(prop.name)
            print(f"  {prop.name}: ({type(value).__name__}) {value} ")
        except Exception as e:
            print(f"  {prop.name}: <error: {e}>")

def on_ssrc_active(rtpbin, session_id, ssrc):

    print("Session:", session_id)
    print("SSRC:", ssrc)

    session = rtpbin.emit("get-internal-session", session_id)
    source = session.emit("get-source-by-ssrc", ssrc)

    #print("RTPSource:", dir(source))
    # source = session.get_source_by_ssrc(ssrc)
    if source:
        # print source type and properties
        print_element_info(source)

        get_ntp_from_source(source)

    # print("RTPSource:", source)

def on_new_manager(rtspsrc, manager):
    print(f"Got RTP manager: {manager.get_name()}, id={id(manager)}")
    #print manager type and properties
    print_element_info(manager)
    manager.connect("on-ssrc-active", on_ssrc_active)


def on_source_setup(bin, source):
    # Check if the source created is rtspsrc
    # You can check by name or by the factory type
    factory = source.get_factory()
    print(f"Source element created: {source.get_name()}, Factory: {factory.get_name() if factory else 'None'}")
    if factory and factory.get_name() == "rtspsrc":
        print("Found rtspsrc! Setting properties...")
        source.connect("new-manager", on_new_manager)

        # Set latency (in milliseconds)
        #source.set_property("latency", 200)

        # Set transport protocol (e.g., force TCP)
        #source.set_property("protocols", 0x00000001)
        # protocols           : Allowed lower transport protocols
        #     flags: readable, writable
        #     Flags "GstRTSPLowerTrans" Default: 0x00000007, "tcp+udp-mcast+udp"
        #         (0x00000000): unknown          - GST_RTSP_LOWER_TRANS_UNKNOWN
        #         (0x00000001): udp              - GST_RTSP_LOWER_TRANS_UDP
        #         (0x00000002): udp-mcast        - GST_RTSP_LOWER_TRANS_UDP_MCAST
        #         (0x00000004): tcp              - GST_RTSP_LOWER_TRANS_TCP
        #         (0x00000010): http             - GST_RTSP_LOWER_TRANS_HTTP
        #         (0x00000020): tls              - GST_RTSP_LOWER_TRANS_TLS

        # Enable NTP synchronization
        source.set_property("ntp-sync", True)

        source.set_property("ntp-time-source", 0)  # Use server time as NTP reference
        # (0): ntp              - NTP time based on realtime clock    
        # (1): unix             - UNIX time based on realtime clock   
        # (2): running-time     - Running time based on pipeline clock
        # (3): clock-time       - Pipeline clock time

        # Ask rtspsrc to attach GstReferenceTimestampMeta with NTP time to each buffer
        # check version
        if Gst.version() >= (1, 22, 0):
            print("Setting add-reference-timestamp-meta to True (GStreamer 1.22+)")
            source.set_property("add-reference-timestamp-meta", True) # newer versions of GStreamer (1.22+)


def get_ntp_timestamp2(buf):

    # Get NTP timestamp via GstReferenceTimestampMeta
    ref_meta = buf.get_reference_timestamp_meta(None)
    
    #print(dir(buf))
    if ref_meta:
        ntp_ts = ref_meta.timestamp        # nanoseconds since 1900
        ntp_sec = ntp_ts / Gst.SECOND
        unix_ts = ntp_sec - 2208988800     # convert NTP->Unix epoch
        dt = datetime.datetime.utcfromtimestamp(unix_ts)
        # print(f"NTP raw:    {ntp_ts}")
        # print(f"Unix time:  {unix_ts:.6f}")
        # print(f"Human time: {dt.isoformat()}")
        return dt.isoformat()

    return 'N/A'

frame_count = 0
def buffer_probe(pad, info):
    """Pad probe that prints PTS and NTP timestamp for every buffer."""
    buf = info.get_buffer()
    if buf is None:
        return Gst.PadProbeReturn.OK

    # PTS (Presentation Timestamp) in the pipeline clock domain
    pts = buf.pts
    pts_str = f"{pts / Gst.SECOND:.6f}s" if pts != Gst.CLOCK_TIME_NONE else "NONE"

    # Get NTP timestamp from buffer metadata
    ntp_str = get_ntp_timestamp2(buf)

    ref_meta = buf.get_reference_timestamp_meta(None)
    global frame_count
    frame_count += 1
    if frame_count % 30 == 0:  # Print every 30 frames
        print(f"[Probe] Frame {frame_count}  PTS={pts_str}  NTP={ntp_str}  RefMeta={'Yes' if ref_meta else 'No'}")

    return Gst.PadProbeReturn.OK


def on_pad_added(element, pad):
    """Attach a buffer probe to newly created video src pads of uridecodebin."""
    caps = pad.get_current_caps() or pad.query_caps(None)
    if caps and not caps.is_empty():
        structure = caps.get_structure(0)
        if structure and structure.get_name().startswith("video/"):
            print(f"[pad-added] Attaching buffer probe to pad: {pad.get_name()}")
            pad.add_probe(Gst.PadProbeType.BUFFER, buffer_probe)



def main():
    # get url from command line
    import sys
    if len(sys.argv) > 1:
        url = sys.argv[1]
        print(f"Using URL from command line: {url}")
    else:
        url = RTSP_URL
        print(f"Using default URL: {url}")

    # Setup the pipeline
    #pipeline_str = f"uridecodebin name=mysrc uri={url} ! autovideosink"
    pipeline_str = f"uridecodebin name=mysrc uri={url} ! fakesink"
    pipeline = Gst.parse_launch(pipeline_str)

    # Retrieve uridecodebin by name
    decodebin = pipeline.get_by_name("mysrc")

    # Connect signals
    decodebin.connect("source-setup", on_source_setup)
    decodebin.connect("pad-added", on_pad_added)

    pipeline.set_state(Gst.State.PLAYING)

    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        pipeline.set_state(Gst.State.NULL)


if __name__ == "__main__":
    main()