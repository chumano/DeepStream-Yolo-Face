Factory Details:
  Rank                     none (0)
  Long-name                RTP packet jitter-buffer
  Klass                    Filter/Network/RTP
  Description              A buffer that deals with network jitter and other transmission faults
  Author                   Philippe Kalaf <philippe.kalaf@collabora.co.uk>, Wim Taymans <wim.taymans@gmail.com>

Plugin Details:
  Name                     rtpmanager
  Description              RTP session management plugin library
  Filename                 /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstrtpmanager.so
  Version                  1.20.3
  License                  LGPL
  Source module            gst-plugins-good
  Source release date      2022-06-15
  Binary package           GStreamer Good Plugins (Ubuntu)
  Origin URL               https://launchpad.net/distros/ubuntu/+source/gst-plugins-good1.0

GObject
 +----GInitiallyUnowned
       +----GstObject
             +----GstElement
                   +----GstRtpJitterBuffer

Pad Templates:
  SINK template: 'sink'
    Availability: Always
    Capabilities:
      application/x-rtp
  
  SINK template: 'sink_rtcp'
    Availability: On request
    Capabilities:
      application/x-rtcp
  
  SRC template: 'src'
    Availability: Always
    Capabilities:
      application/x-rtp

Clocking Interaction:
  element is supposed to provide a clock but returned NULL
Element has no URI handling capabilities.

Pads:
  SRC: 'src'
    Pad Template: 'src'
  SINK: 'sink'
    Pad Template: 'sink'

Element Properties:
  do-lost             : Send an event downstream when a packet is lost
                        flags: readable, writable
                        Boolean. Default: false
  do-retransmission   : Send retransmission events upstream when a packet is late
                        flags: readable, writable
                        Boolean. Default: false
  drop-messages-interval: Minimal time between posting dropped packet messages
                        flags: readable, writable
                        Unsigned Integer. Range: 0 - 4294967295 Default: 200 
  drop-on-latency     : Tells the jitterbuffer to never exceed the given latency in size
                        flags: readable, writable
                        Boolean. Default: false
  faststart-min-packets: The number of consecutive packets needed to start (set to 0 to disable faststart. The jitterbuffer will by default start after the latency has elapsed)
                        flags: readable, writable
                        Unsigned Integer. Range: 0 - 4294967295 Default: 0 
  latency             : Amount of ms to buffer
                        flags: readable, writable
                        Unsigned Integer. Range: 0 - 4294967295 Default: 200 
  max-dropout-time    : The maximum time (milliseconds) of missing packets tolerated.
                        flags: readable, writable
                        Unsigned Integer. Range: 0 - 2147483647 Default: 60000 
  max-misorder-time   : The maximum time (milliseconds) of misordered packets tolerated.
                        flags: readable, writable
                        Unsigned Integer. Range: 0 - 4294967295 Default: 2000 
  max-rtcp-rtp-time-diff: Maximum amount of time in ms that the RTP time in RTCP SRs is allowed to be ahead (-1 disabled)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: 1000 
  max-ts-offset-adjustment: The maximum number of nanoseconds per frame that time stamp offsets may be adjusted (0 = no limit).
                        flags: readable, writable
                        Unsigned Integer64. Range: 0 - 18446744073709551615 Default: 0 
  mode                : Control the buffering algorithm in use
                        flags: readable, writable
                        Enum "RTPJitterBufferMode" Default: 1, "slave"
                           (0): none             - Only use RTP timestamps
                           (1): slave            - Slave receiver to sender clock
                           (2): buffer           - Do low/high watermark buffering
                           (4): synced           - Synchronized sender and receiver clocks
  name                : The name of the object
                        flags: readable, writable, 0x2000
                        String. Default: "rtpjitterbuffer0"
  parent              : The parent of the object
                        flags: readable, writable, 0x2000
                        Object of type "GstObject"
  percent             : The buffer filled percent
                        flags: readable
                        Integer. Range: 0 - 100 Default: 0 
  post-drop-messages  : Post a custom message to the bus when a packet is dropped by the jitterbuffer
                        flags: readable, writable
                        Boolean. Default: false
  rfc7273-sync        : Synchronize received streams to the RFC7273 clock (requires clock and offset to be provided)
                        flags: readable, writable
                        Boolean. Default: false
  rtx-deadline        : The deadline for a valid RTX request in milliseconds. (-1 automatic)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: -1 
  rtx-delay           : Extra time in ms to wait before sending retransmission event (-1 automatic)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: -1 
  rtx-delay-reorder   : Sending retransmission event when this much reordering (0 disable)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: 3 
  rtx-max-retries     : The maximum number of retries to request a retransmission. (-1 not limited)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: -1 
  rtx-min-delay       : Minimum time in ms to wait before sending retransmission event
                        flags: readable, writable
                        Unsigned Integer. Range: 0 - 4294967295 Default: 0 
  rtx-min-retry-timeout: Minimum timeout between sending a transmission event in ms (-1 automatic)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: -1 
  rtx-next-seqnum     : Estimate when the next packet should arrive and schedule a retransmission request for it.
                        flags: readable, writable
                        Boolean. Default: true
  rtx-retry-period    : Try to get a retransmission for this many ms (-1 automatic)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: -1 
  rtx-retry-timeout   : Retry sending a transmission event after this timeout in ms (-1 automatic)
                        flags: readable, writable
                        Integer. Range: -1 - 2147483647 Default: -1 
  rtx-stats-timeout   : The time to wait for a retransmitted packet after it has been considered lost in order to collect statistics (ms)
                        flags: readable, writable
                        Unsigned Integer. Range: 0 - 4294967295 Default: 1000 
  stats               : Various statistics
                        flags: readable
                        Boxed pointer of type "GstStructure"
                                                          num-pushed: 0 Number of RTP packets successfully pushed downstream.
                                                            num-lost: 0 Number of packets detected as lost (not received in time).
                                                            num-late: 0 Number of packets that arrived too late to be useful.
                                                      num-duplicates: 0 Number of duplicate packets received.
                                                          avg-jitter: 0 Average network jitter (variation in packet arrival time).
                                                           rtx-count: 0 Number of retransmission requests sent.
                                                     rtx-success-count: 0 Number of retransmission requests that were successful.
                                                      rtx-per-packet: 0 Average number of retransmission requests per packet
                                                             rtx-rtt: 0 Round-trip time for retransmission requests (in ms).

  ts-offset           : Adjust buffer timestamps with offset in nanoseconds
                        flags: readable, writable
                        Integer64. Range: -9223372036854775808 - 9223372036854775807 Default: 0 

Element Signals:
  "request-pt-map" :  GstCaps* user_function (GstElement* object,
                                               guint arg0,
                                               gpointer user_data);
  "handle-sync" :  void user_function (GstElement* object,
                                       GstStructure* arg0,
                                       gpointer user_data);
  "on-npt-stop" :  void user_function (GstElement* object,
                                       gpointer user_data);

Element Actions:
  "clear-pt-map" :  void user_function (GstElement* object);
  "set-active" :  guint64 user_function (GstElement* object,
                                         gboolean arg0,
                                         guint64 arg1);
