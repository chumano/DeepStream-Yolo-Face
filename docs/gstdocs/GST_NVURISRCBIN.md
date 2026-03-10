Factory Details:
  Rank                     primary (256)
  Long-name                NvUriSrc Bin
  Klass                    NvUriSrc Bin
  Description              Nvidia DeepStreamSDK NvUriSrc Bin
  Author                   NVIDIA Corporation. Post on Deepstream for Tesla forum for any queries @ https://devtalk.nvidia.com/default/board/209/

Plugin Details:
  Name                     nvdsgst_nvurisrcbin
  Description              Deepstream SDK nvurisrcbin Bin
  Filename                 /opt/nvidia/deepstream/deepstream/lib/gst-plugins/libnvdsgst_nvurisrcbin.so
  Version                  7.1.0
  License                  Proprietary
  Source module            DeepStream SDK nvurisrcbin Bin
  Binary package           Deepstream SDK nvurisrcbin Bin
  Origin URL               http://nvidia.com/

GObject
 +----GInitiallyUnowned
       +----GstObject
             +----GstElement
                   +----GstBin
                         +----GstDsNvUriSrcBin

Implemented Interfaces:
  GstChildProxy

Pad Templates:
  SRC template: 'asrc_%u'
    Availability: Sometimes
    Capabilities:
      audio/x-raw
                 format: { (string)F64LE, (string)F64BE, (string)F32LE, (string)F32BE, (string)S32LE, (string)S32BE, (string)U32LE, (string)U32BE, (string)S24_32LE, (string)S24_32BE, (string)U24_32LE, (string)U24_32BE, (string)S24LE, (string)S24BE, (string)U24LE, (string)U24BE, (string)S20LE, (string)S20BE, (string)U20LE, (string)U20BE, (string)S18LE, (string)S18BE, (string)U18LE, (string)U18BE, (string)S16LE, (string)S16BE, (string)U16LE, (string)U16BE, (string)S8, (string)U8 }
                   rate: [ 1, 2147483647 ]
               channels: [ 1, 2147483647 ]
  
  SRC template: 'vsrc_%u'
    Availability: Sometimes
    Capabilities:
      video/x-raw(memory:NVMM)
                 format: { (string)I420, (string)NV12, (string)P010_10LE, (string)BGRx, (string)RGBA, (string)GRAY8 }
                  width: [ 1, 2147483647 ]
                 height: [ 1, 2147483647 ]
              framerate: [ 0/1, 2147483647/1 ]
      video/x-raw
                 format: { (string)I420, (string)P010_10LE, (string)NV12, (string)BGRx, (string)RGBA, (string)GRAY8 }
                  width: [ 1, 2147483647 ]
                 height: [ 1, 2147483647 ]
              framerate: [ 0/1, 2147483647/1 ]

Element has no clocking capabilities.
Element has no URI handling capabilities.

Pads:
  none

Element Properties:
  async-handling      : The bin will handle Asynchronous state changes
                        flags: readable, writable
                        Boolean. Default: true
  cudadec-memtype     : Set to specify memory type for cuda decoder buffers
                        flags: readable, writable, changeable only in NULL or READY state
                        Enum "GstNvUriSrcBinCudaDecMemType" Default: 0, "memtype_device"
                           (0): memtype_device   - Memory type Device
                           (1): memtype_pinned   - Memory type Host Pinned
                           (2): memtype_unified  - Memory type Unified
  dec-skip-frames     : Type of frames to skip during decoding
                        flags: readable, writable, changeable only in NULL or READY state
                        Enum "SkipFrames" Default: 0, "decode_all"
                           (0): decode_all       - Decode all frames
                           (1): decode_non_ref   - Decode non-ref frames
                           (2): decode_key       - decode key frames
  disable-audio       : Disable audio path mode at init time
                        flags: readable, writable
                        Boolean. Default: true
  disable-passthrough : Disable passthrough mode at init time, applicable for nvvideoconvert only.
                        flags: readable, writable
                        Boolean. Default: true
  drop-frame-interval : Interval to drop the frames,ex: value of 5 means every 5th frame will be given by decoder, rest all dropped
                        flags: readable, writable, changeable only in NULL or READY state
                        Unsigned Integer. Range: 0 - 30 Default: 0 
  drop-on-latency     : Tells the jitterbuffer to never exceed the given latency in size
                        flags: readable, writable
                        Boolean. Default: true
  extract-sei-type5-data: Set to extract and attach SEI type5 unregistered data on output buffer
                        flags: readable, writable
                        Boolean. Default: false
  file-loop           : Loop file sources after EOS. Src type must be source-type-uri and uri starting with 'file:/'
                        flags: readable, writable
                        Boolean. Default: false
  gpu-id              : Set GPU Device ID
                        flags: readable, writable, changeable only in NULL or READY state
                        Unsigned Integer. Range: 0 - 4294967295 Default: 0 
  ipc-buffer-timestamp-copy: Copy buffer timestamp for nvunixfdsrc plugin
                        flags: readable, writable
                        Boolean. Default: false
  ipc-connection-attempts: Max number of attempts for connection (-1 = unlimited)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: -1 
  ipc-connection-interval: connection interval between connection attempts in micro seconds
                        flags: readable, writable
                        Unsigned Integer64. Range: 0 - 18446744073709551615 Default: 1000000 
  ipc-socket-path     : The path to the control socket used to control the shared memory transport. This may be modified during the NULL->READY transition
                        flags: readable, writable, changeable only in NULL or READY state
                        String. Default: null
  latency             : Jitterbuffer size in milliseconds; applicable only for RTSP streams.
                        flags: readable, writable, changeable only in NULL or READY state
                        Unsigned Integer. Range: 0 - 4294967295 Default: 100 
  low-latency-mode    : Set low latency mode for bitstreams having I and IPPP frames on decoder
                        flags: readable, writable
                        Boolean. Default: false
  message-forward     : Forwards all children messages
                        flags: readable, writable
                        Boolean. Default: false
  name                : The name of the object
                        flags: readable, writable, 0x2000
                        String. Default: "dsnvurisrcbin0"
  num-extra-surfaces  : Number of surfaces in addition to minimum decode surfaces given by the decoder
                        flags: readable, writable, changeable only in NULL or READY state
                        Unsigned Integer. Range: 0 - 4294967295 Default: 1 
  parent              : The parent of the object
                        flags: readable, writable, 0x2000
                        Object of type "GstObject"
  rtsp-reconnect-attempts: Set rtsp reconnect attempt value
                        flags: readable, writable, changeable only in NULL or READY state
                        Integer. Range: -2147483648 - 2147483647 Default: -1 
  rtsp-reconnect-interval: Timeout in seconds to wait since last data was received from an RTSP source before forcing a reconnection. 0=disable timeout
                        flags: readable, writable, changeable only in NULL or READY state
                        Unsigned Integer. Range: 0 - 4294967295 Default: 0 
  sei-uuid            : Set sei uuid on decoder
                        flags: readable, writable, changeable only in NULL or READY state
                        String. Default: null
  select-rtp-protocol : Transport Protocol to use for RTP
                        flags: readable, writable, changeable only in NULL or READY state
                        Enum "RtpProtocol" Default: 0, "rtp-multi"
                           (0): rtp-multi        - UDP + UDP Multicast + TCP
                           (4): rtp-tcp          - TCP Only
  smart-rec-cache     : Size of cache in seconds, applies to both audio and video cache
                        flags: readable, writable, changeable only in NULL or READY state
                        Unsigned Integer. Range: 0 - 4294967295 Default: 0 
  smart-rec-container : Container format of recorded video. MP4 and MKV containers are supported. Sources must be of type source-type-rtsp
                        flags: readable, writable
                        Enum "SmartRecordContainerType" Default: 0, "smart-rec-mp4"
                           (0): smart-rec-mp4    - MP4 container
                           (1): smart-rec-mkv    - MKV container
  smart-rec-default-duration: In case a Stop event is not generated. This parameter will ensure the recording is stopped after a predefined default duration.
                        flags: readable, writable, changeable only in NULL or READY state
                        Unsigned Integer. Range: 0 - 4294967295 Default: 20 
  smart-rec-dir-path  : Path of directory to save the recorded file.
                        flags: readable, writable, changeable only in NULL or READY state
                        String. Default: null
  smart-rec-file-prefix: By default, Smart_Record is the prefix. For unique file names every source must be provided with a unique prefix
                        flags: readable, writable, changeable only in NULL or READY state
                        String. Default: "Smart_Record"
  smart-rec-mode      : Smart record mode
                        flags: readable, writable
                        Enum "SmartRecordMode" Default: 0, "smart-rec-mode-av"
                           (0): smart-rec-mode-av - Record audio and video if available
                           (1): smart-rec-mode-video - Record video only if available
                           (2): smart-rec-mode-audio - Record audio only if available
  smart-rec-status    : Boolean indicating if SR is currently
                        flags: readable
                        Boolean. Default: false
  smart-rec-video-cache: Size of video cache in seconds. DEPRECATED: Use 'smart-rec-cache' instead
                        flags: readable, writable, changeable only in NULL or READY state
                        Unsigned Integer. Range: 0 - 4294967295 Default: 0 
  smart-record        : Enable Smart Record and choose the type of events to respond to. Sources must be of type source-type-rtsp
                        flags: readable, writable
                        Enum "SmartRecordType" Default: 0, "smart-rec-disable"
                           (0): smart-rec-disable - Disable Smart Record
                           (1): smart-rec-cloud  - Trigger Smart Record through cloud messages only
                           (2): smart-rec-multi  - Trigger Smart Record through cloud and local events
  source-id           : Unique ID for the input source
                        flags: readable, writable, changeable only in NULL or READY state
                        Integer. Range: -1 - 2147483647 Default: -1 
  type                : Set the type of source. Use source-type-rtsp to use smart record features
                        flags: readable, writable, changeable only in NULL or READY state
                        Enum "GstNvSourceType" Default: 0, "auto"
                           (0): auto             - Select source type based on URI scheme
                           (1): uri              - Supports any URI supported by GStreamer
                           (2): rtsp             - Customize for RTSP, supports smart recording
  udp-buffer-size     : UDP Buffer Size in bytes; applicable only for RTSP streams.
                        flags: readable, writable, changeable only in NULL or READY state
                        Unsigned Integer. Range: 0 - 4294967295 Default: 524288 
  uri                 : URI of the file or rtsp source
                        flags: readable, writable, changeable only in NULL or READY state
                        String. Default: null

Element Signals:
  "pad-added" :  void user_function (GstElement* object,
                                     GstPad* arg0,
                                     gpointer user_data);
  "pad-removed" :  void user_function (GstElement* object,
                                       GstPad* arg0,
                                       gpointer user_data);
  "no-more-pads" :  void user_function (GstElement* object,
                                        gpointer user_data);
  "sr-done" :  void user_function (GstElement* object,
                                   gpointer arg0,
                                   gpointer arg1,
                                   gpointer user_data);

Element Actions:
  "start-sr" :  void user_function (GstElement* object,
                                    gpointer arg0,
                                    guint arg1,
                                    guint arg2,
                                    gpointer arg3);
  "stop-sr" :  void user_function (GstElement* object,
                                   guint arg0);
