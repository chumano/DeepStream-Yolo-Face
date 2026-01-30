import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib

import os
import sys
import time
import argparse
import platform
from threading import Lock
from ctypes import sizeof, c_float

sys.path.append("/opt/nvidia/deepstream/deepstream/lib")
import pyds

#MAX_ELEMENTS_IN_DISPLAY_META = 16

# SOURCE = ""
# INFER_CONFIG = ""
# STREAMMUX_BATCH_SIZE = 1
# STREAMMUX_WIDTH = 1920
# STREAMMUX_HEIGHT = 1080
# GPU_ID = 0

# PERF_MEASUREMENT_INTERVAL_SEC = 5
# JETSON = False

perf_struct = {}

def main():
    print(os.environ.get("LD_LIBRARY_PATH"))
    print(os.environ.get("GST_PLUGIN_PATH"))
    Gst.init(None)

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

    result = pipeline.add(nvstreammux)
    print(f"pipeline.add() returned: {result}, type: {type(result)}")
    print(f"pipeline type: {type(pipeline)}")
    print(f"nvstreammux type: {type(nvstreammux)}")
    print(f"Is pipeline a Gst.Bin? {isinstance(pipeline, Gst.Bin)}")
    print(f"Is nvstreammux a Gst.Element? {isinstance(nvstreammux, Gst.Element)}")
    
    if result is False:
        sys.stderr.write("ERROR - Failed to add nvstreammux to pipeline\n")
        return -1
    
    print("Successfully added nvstreammux to pipeline")

    # check  pyds
    if pyds is not None:
        print("pyds module is available")
    dirs = dir(pyds)
    print("pyds module attributes and methods:")
    for d in dirs:
        print(d)
    print(f"pyds version: {pyds.__version__}")
    
if __name__ == "__main__":
    main()