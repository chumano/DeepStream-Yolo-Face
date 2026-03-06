`  gst-inspect-1.0 videotestsrc > GST_videotestsrc.md `

Factory Details:
  Rank                     none (0)
  Long-name                Video test source
  Klass                    Source/Video
  Description              Creates a test video stream
  Author                   David A. Schleef <ds@schleef.org>

Plugin Details:
  Name                     videotestsrc
  Description              Creates a test video stream
  Filename                 /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstvideotestsrc.so
  Version                  1.20.1
  License                  LGPL
  Source module            gst-plugins-base
  Source release date      2022-03-14
  Binary package           GStreamer Base Plugins (Ubuntu)
  Origin URL               https://launchpad.net/distros/ubuntu/+source/gst-plugins-base1.0

GObject
 +----GInitiallyUnowned
       +----GstObject
             +----GstElement
                   +----GstBaseSrc
                         +----GstPushSrc
                               +----GstVideoTestSrc

Pad Templates:
  SRC template: 'src'
    Availability: Always
    Capabilities:
      video/x-raw
                 format: { (string)ABGR64_LE, (string)BGRA64_LE, (string)AYUV64, (string)ARGB64_LE, (string)ARGB64, (string)RGBA64_LE, (string)ABGR64_BE, (string)BGRA64_BE, (string)ARGB64_BE, (string)RGBA64_BE, (string)GBRA_12LE, (string)GBRA_12BE, (string)Y412_LE, (string)Y412_BE, (string)A444_10LE, (string)GBRA_10LE, (string)A444_10BE, (string)GBRA_10BE, (string)A422_10LE, (string)A422_10BE, (string)A420_10LE, (string)A420_10BE, (string)RGB10A2_LE, (string)BGR10A2_LE, (string)Y410, (string)GBRA, (string)ABGR, (string)VUYA, (string)BGRA, (string)AYUV, (string)ARGB, (string)RGBA, (string)A420, (string)AV12, (string)Y444_16LE, (string)Y444_16BE, (string)v216, (string)P016_LE, (string)P016_BE, (string)Y444_12LE, (string)GBR_12LE, (string)Y444_12BE, (string)GBR_12BE, (string)I422_12LE, (string)I422_12BE, (string)Y212_LE, (string)Y212_BE, (string)I420_12LE, (string)I420_12BE, (string)P012_LE, (string)P012_BE, (string)Y444_10LE, (string)GBR_10LE, (string)Y444_10BE, (string)GBR_10BE, (string)r210, (string)I422_10LE, (string)I422_10BE, (string)NV16_10LE32, (string)Y210, (string)v210, (string)UYVP, (string)I420_10LE, (string)I420_10BE, (string)P010_10LE, (string)NV12_10LE32, (string)NV12_10LE40, (string)P010_10BE, (string)Y444, (string)RGBP, (string)GBR, (string)BGRP, (string)NV24, (string)xBGR, (string)BGRx, (string)xRGB, (string)RGBx, (string)BGR, (string)IYU2, (string)v308, (string)RGB, (string)Y42B, (string)NV61, (string)NV16, (string)VYUY, (string)UYVY, (string)YVYU, (string)YUY2, (string)I420, (string)YV12, (string)NV21, (string)NV12, (string)NV12_64Z32, (string)NV12_4L4, (string)NV12_32L32, (string)Y41B, (string)IYU1, (string)YVU9, (string)YUV9, (string)RGB16, (string)BGR16, (string)RGB15, (string)BGR15, (string)RGB8P, (string)GRAY16_LE, (string)GRAY16_BE, (string)GRAY10_LE32, (string)GRAY8 }
                  width: [ 1, 2147483647 ]
                 height: [ 1, 2147483647 ]
              framerate: [ 0/1, 2147483647/1 ]
         multiview-mode: { (string)mono, (string)left, (string)right }
      video/x-bayer
                 format: { (string)bggr, (string)rggb, (string)grbg, (string)gbrg }
                  width: [ 1, 2147483647 ]
                 height: [ 1, 2147483647 ]
              framerate: [ 0/1, 2147483647/1 ]
         multiview-mode: { (string)mono, (string)left, (string)right }

Element has no clocking capabilities.
Element has no URI handling capabilities.

Pads:
  SRC: 'src'
    Pad Template: 'src'

Element Properties:
  animation-mode      : For pattern=ball, which counter defines the position of the ball.
                        flags: readable, writable
                        Enum "GstVideoTestSrcAnimationMode" Default: 0, "frames"
                           (0): frames           - frame count
                           (1): wall-time        - wall clock time
                           (2): running-time     - running time
  background-color    : Background color to use (big-endian ARGB)
                        flags: readable, writable, controllable
                        Unsigned Integer. Range: 0 - 4294967295 Default: 4278190080 
  blocksize           : Size in bytes to read per buffer (-1 = default)
                        flags: readable, writable
                        Unsigned Integer. Range: 0 - 4294967295 Default: 4096 
  do-timestamp        : Apply current stream time to buffers
                        flags: readable, writable
                        Boolean. Default: false
  flip                : For pattern=ball, invert colors every second.
                        flags: readable, writable
                        Boolean. Default: false
  foreground-color    : Foreground color to use (big-endian ARGB)
                        flags: readable, writable, controllable
                        Unsigned Integer. Range: 0 - 4294967295 Default: 4294967295 
  horizontal-speed    : Scroll image number of pixels per frame (positive is scroll to the left)
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  is-live             : Whether to act as a live source
                        flags: readable, writable
                        Boolean. Default: false
  k0                  : Zoneplate zero order phase, for generating plain fields or phase offsets
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  kt                  : Zoneplate 1st order t phase, for generating phase rotation as a function of time
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  kt2                 : Zoneplate 2nd order t phase, t*t/256 cycles per picture
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  kx                  : Zoneplate 1st order x phase, for generating constant horizontal frequencies
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  kx2                 : Zoneplate 2nd order x phase, normalised to kx2/256 cycles per horizontal pixel at width/2 from origin
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  kxt                 : Zoneplate x*t product phase, normalised to kxy/256 cycles per vertical pixel at width/2 from origin
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  kxy                 : Zoneplate x*y product phase
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  ky                  : Zoneplate 1st order y phase, for generating content vertical frequencies
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  ky2                 : Zoneplate 2nd order y phase, normailsed to ky2/256 cycles per vertical pixel at height/2 from origin
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  kyt                 : Zoneplate y*t product phase
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  motion              : For pattern=ball, what motion the ball does
                        flags: readable, writable
                        Enum "GstVideoTestSrcMotionType" Default: 0, "wavy"
                           (0): wavy             - Ball waves back and forth, up and down
                           (1): sweep            - 1 revolution per second
                           (2): hsweep           - 1/2 revolution per second, then reset to top
  name                : The name of the object
                        flags: readable, writable, 0x2000
                        String. Default: "videotestsrc0"
  num-buffers         : Number of buffers to output before sending EOS (-1 = unlimited)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: -1 
  parent              : The parent of the object
                        flags: readable, writable, 0x2000
                        Object of type "GstObject"
  pattern             : Type of test pattern to generate
                        flags: readable, writable
                        Enum "GstVideoTestSrcPattern" Default: 0, "smpte"
                           (0): smpte            - SMPTE 100% color bars
                           (1): snow             - Random (television snow)
                           (2): black            - 100% Black
                           (3): white            - 100% White
                           (4): red              - Red
                           (5): green            - Green
                           (6): blue             - Blue
                           (7): checkers-1       - Checkers 1px
                           (8): checkers-2       - Checkers 2px
                           (9): checkers-4       - Checkers 4px
                           (10): checkers-8       - Checkers 8px
                           (11): circular         - Circular
                           (12): blink            - Blink
                           (13): smpte75          - SMPTE 75% color bars
                           (14): zone-plate       - Zone plate
                           (15): gamut            - Gamut checkers
                           (16): chroma-zone-plate - Chroma zone plate
                           (17): solid-color      - Solid color
                           (18): ball             - Moving ball
                           (19): smpte100         - SMPTE 100% color bars
                           (20): bar              - Bar
                           (21): pinwheel         - Pinwheel
                           (22): spokes           - Spokes
                           (23): gradient         - Gradient
                           (24): colors           - Colors
                           (25): smpte-rp-219     - SMPTE test pattern, RP 219 conformant
  timestamp-offset    : An offset added to timestamps set on buffers (in ns)
                        flags: readable, writable
                        Integer64. Range: 0 - 9223372036854775807 Default: 0 
  typefind            : Run typefind before negotiating (deprecated, non-functional)
                        flags: readable, writable, deprecated
                        Boolean. Default: false
  xoffset             : Zoneplate 2nd order products x offset
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
  yoffset             : Zoneplate 2nd order products y offset
                        flags: readable, writable
                        Integer. Range: -2147483648 - 2147483647 Default: 0 
