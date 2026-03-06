# GStreamer Event Types Documentation

This document provides a comprehensive overview of all event types in GStreamer (`GstEventType`).

## Table of Contents

1. [Overview](#overview)
2. [Event Flags](#event-flags)
3. [Bidirectional Events](#bidirectional-events)
4. [Downstream Serialized Events](#downstream-serialized-events)
5. [Non-Sticky Downstream Serialized Events](#non-sticky-downstream-serialized-events)
6. [Sticky Downstream Non-Serialized Events](#sticky-downstream-non-serialized-events)
7. [Upstream Events](#upstream-events)
8. [Custom Events](#custom-events)

## Overview

GStreamer events are used to communicate control information between elements in a pipeline. Events flow either upstream (towards the source) or downstream (towards the sink), and can be serialized with the data flow or sent out-of-band.

## Event Flags

Events have flags that determine their behavior:

- **`GST_EVENT_TYPE_UPSTREAM`**: Event can travel upstream
- **`GST_EVENT_TYPE_DOWNSTREAM`**: Event can travel downstream
- **`GST_EVENT_TYPE_SERIALIZED`**: Event is serialized with data flow
- **`GST_EVENT_TYPE_STICKY`**: Event stays on the pad until replaced
- **`GST_EVENT_TYPE_STICKY_MULTI`**: Multiple sticky events of this type can exist

## Bidirectional Events

### GST_EVENT_FLUSH_START (10)

**Flags**: `BOTH`

Starts a flush operation. This event:
- Clears all data from the pipeline
- Unblocks all streaming threads
- Puts pads into flushing mode
- Can travel both upstream and downstream

**Usage**: Typically sent when seeking or stopping playback to clear buffered data.

```c
GstEvent *event = gst_event_new_flush_start();
gst_pad_push_event(pad, event);
```

### GST_EVENT_FLUSH_STOP (20)

**Flags**: `BOTH | SERIALIZED`

Stops a flush operation. This event:
- Exits flushing mode
- Optionally resets the running time
- Allows data flow to resume
- Serialized with data stream

**Usage**: Sent after `FLUSH_START` to resume normal operation.

```c
GstEvent *event = gst_event_new_flush_stop(TRUE); // reset_time = TRUE
gst_pad_push_event(pad, event);
```

## Downstream Serialized Events

### GST_EVENT_STREAM_START (40)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY`

Marks the start of a new stream. This event:
- Must be sent before any other serialized event
- Contains a unique stream ID
- Can include group ID and stream flags
- Sent at stream start, not after seeking

**Usage**: First event in any stream.

```c
GstEvent *event = gst_event_new_stream_start("stream-id-12345");
gst_event_set_group_id(event, 42);
```

### GST_EVENT_CAPS (50)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY`

Notifies downstream elements of the media type. This event:
- Contains the negotiated caps
- Must be sent before buffers
- Triggers pad reconfiguration if caps change

**Usage**: After caps negotiation.

```c
GstCaps *caps = gst_caps_new_simple("video/x-raw", 
    "format", G_TYPE_STRING, "I420", NULL);
GstEvent *event = gst_event_new_caps(caps);
gst_caps_unref(caps);
```

### GST_EVENT_SEGMENT (70)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY`

Defines a media segment. This event:
- Contains clipping and timestamp conversion information
- Defines start/stop positions and playback rate
- Required before sending buffers

**Usage**: After CAPS event, before buffers.

```c
GstSegment segment;
gst_segment_init(&segment, GST_FORMAT_TIME);
GstEvent *event = gst_event_new_segment(&segment);
```

### GST_EVENT_STREAM_COLLECTION (75)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY | STICKY_MULTI`

**Since**: 1.10

Announces available streams. This event:
- Contains a `GstStreamCollection`
- Used for stream selection
- Multiple collections can exist

**Usage**: Demuxers send this to announce available tracks.

```c
GstStreamCollection *collection = gst_stream_collection_new("collection-id");
// Add streams to collection
GstEvent *event = gst_event_new_stream_collection(collection);
```

### GST_EVENT_TAG (80)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY | STICKY_MULTI`

Carries metadata tags. This event:
- Contains a `GstTagList` with metadata
- Can be sent multiple times with different tags
- Merged or replaced based on tag merge mode

**Usage**: Send metadata like title, artist, etc.

```c
GstTagList *tags = gst_tag_list_new(GST_TAG_TITLE, "Song Title", NULL);
GstEvent *event = gst_event_new_tag(tags);
```

### GST_EVENT_BUFFERSIZE (90)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY`

Notifies of buffering requirements. This event:
- Contains minimum and maximum buffer sizes
- Currently not widely used
- Reserved for future use

**Usage**: Advanced buffering control.

```c
GstEvent *event = gst_event_new_buffer_size(
    GST_FORMAT_BYTES, 4096, 65536, FALSE);
```

### GST_EVENT_SINK_MESSAGE (100)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY | STICKY_MULTI`

Carries messages to be emitted in sync with rendering. This event:
- Contains a `GstMessage`
- Sinks convert this to a bus message
- Synchronized with data flow

**Usage**: Send messages that must be synchronized with playback.

```c
GstMessage *msg = gst_message_new_application(...);
GstEvent *event = gst_event_new_sink_message("msg-name", msg);
```

### GST_EVENT_STREAM_GROUP_DONE (105)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY`

**Since**: 1.10

Indicates no more data for a stream group. This event:
- Sent before EOS in some scenarios
- Signals end of a stream group
- Handled similarly to EOS

**Usage**: Multi-stream scenarios with stream groups.

```c
GstEvent *event = gst_event_new_stream_group_done(group_id);
```

### GST_EVENT_EOS (110)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY`

End-Of-Stream event. This event:
- Signals no more data will follow
- Must be preceded by STREAM_START or SEGMENT
- Triggers pipeline shutdown or segment completion

**Usage**: End of stream or file.

```c
GstEvent *event = gst_event_new_eos();
gst_pad_push_event(pad, event);
```

### GST_EVENT_TOC (120)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY | STICKY_MULTI`

Table of Contents event. This event:
- Contains chapter/track information
- Can be sent multiple times if TOC changes
- Used for navigation

**Usage**: Media with chapters or tracks.

```c
GstToc *toc = gst_toc_new(GST_TOC_SCOPE_GLOBAL);
// Build TOC structure
GstEvent *event = gst_event_new_toc(toc, FALSE);
```

### GST_EVENT_PROTECTION (130)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY | STICKY_MULTI`

Carries encryption/DRM information. This event:
- Contains system ID and protection data
- Used for encrypted content
- Multiple protection events possible

**Usage**: DRM-protected content.

```c
GstEvent *event = gst_event_new_protection(
    "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed", 
    data_buffer, "dash");
```

## Non-Sticky Downstream Serialized Events

### GST_EVENT_SEGMENT_DONE (150)

**Flags**: `DOWNSTREAM | SERIALIZED`

Marks the end of a segment. This event:
- Sent when segment playback completes
- Used in segment seeks
- Not sticky

**Usage**: After segment playback.

```c
GstEvent *event = gst_event_new_segment_done(GST_FORMAT_TIME, position);
```

### GST_EVENT_GAP (160)

**Flags**: `DOWNSTREAM | SERIALIZED`

Marks a gap in the data stream. This event:
- Signals missing or silent data
- Contains timestamp and duration
- Can indicate packet loss

**Usage**: When data is missing or intentionally skipped.

```c
GstEvent *event = gst_event_new_gap(timestamp, duration);
gst_event_set_gap_flags(event, GST_GAP_FLAG_MISSING_DATA);
```

## Sticky Downstream Non-Serialized Events

### GST_EVENT_INSTANT_RATE_CHANGE (180)

**Flags**: `DOWNSTREAM | STICKY`

**Since**: 1.18

Applies an instant rate change. This event:
- Changes playback rate immediately
- Non-serialized (takes effect instantly)
- Does not require flushing

**Usage**: Smooth rate transitions.

```c
GstEvent *event = gst_event_new_instant_rate_change(
    2.0, GST_SEGMENT_FLAG_NONE);
```

## Upstream Events

### GST_EVENT_QOS (190)

**Flags**: `UPSTREAM`

Quality of Service event. This event:
- Sent by sinks to report performance
- Contains proportion, diff, and timestamp
- Helps upstream adjust processing

**Usage**: Automatic quality adjustment.

```c
GstEvent *event = gst_event_new_qos(
    GST_QOS_TYPE_OVERFLOW, 0.8, -10 * GST_MSECOND, timestamp);
```

### GST_EVENT_SEEK (200)

**Flags**: `UPSTREAM`

Requests a new playback position. This event:
- Specifies rate, format, and position
- Can be flushing or non-flushing
- Supports various seek types

**Usage**: User-initiated seeking.

```c
GstEvent *event = gst_event_new_seek(
    1.0, GST_FORMAT_TIME, 
    GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
    GST_SEEK_TYPE_SET, 5 * GST_SECOND,
    GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
```

### GST_EVENT_NAVIGATION (210)

**Flags**: `UPSTREAM`

User navigation event. This event:
- Carries mouse/keyboard events
- Used for DVD menus, interactive content
- Custom structure defines event details

**Usage**: User interaction in video overlays.

```c
GstStructure *s = gst_structure_new("application/x-gst-navigation",
    "event", G_TYPE_STRING, "mouse-button-press", NULL);
GstEvent *event = gst_event_new_navigation(s);
```

### GST_EVENT_LATENCY (220)

**Flags**: `UPSTREAM`

Latency notification. This event:
- Reconfigures pipeline latency
- Sent when latency changes
- Triggers latency recalculation

**Usage**: Dynamic latency adjustment.

```c
GstEvent *event = gst_event_new_latency(50 * GST_MSECOND);
```

### GST_EVENT_STEP (230)

**Flags**: `UPSTREAM`

Frame-stepping request. This event:
- Advances by specified amount
- Can step frames, time, or buffers
- Used for frame-by-frame playback

**Usage**: Debug or analysis tools.

```c
GstEvent *event = gst_event_new_step(
    GST_FORMAT_BUFFERS, 1, 1.0, TRUE, FALSE);
```

### GST_EVENT_RECONFIGURE (240)

**Flags**: `UPSTREAM`

Requests upstream reconfiguration. This event:
- Triggers caps renegotiation
- Sent when downstream requirements change
- Causes upstream to reconsider output format

**Usage**: Dynamic format changes.

```c
GstEvent *event = gst_event_new_reconfigure();
gst_pad_push_event(pad, event);
```

### GST_EVENT_TOC_SELECT (250)

**Flags**: `UPSTREAM`

Selects a TOC entry. This event:
- Seeks to a specific chapter/track
- Contains TOC entry UID
- Alternative to time-based seeking

**Usage**: Chapter navigation.

```c
GstEvent *event = gst_event_new_toc_select("chapter-03");
```

### GST_EVENT_SELECT_STREAMS (260)

**Flags**: `UPSTREAM`

**Since**: 1.10

Selects active streams. This event:
- Contains list of stream IDs to activate
- Used with `STREAM_COLLECTION`
- Enables stream switching

**Usage**: Audio/subtitle track selection.

```c
GList *streams = NULL;
streams = g_list_append(streams, g_strdup("video-stream-id"));
streams = g_list_append(streams, g_strdup("audio-stream-id-2"));
GstEvent *event = gst_event_new_select_streams(streams);
```

### GST_EVENT_INSTANT_RATE_SYNC_TIME (261)

**Flags**: `UPSTREAM`

**Since**: 1.18

Synchronizes instant rate changes. This event:
- Sent by pipeline to notify of rate change timing
- Contains running time when rate was applied
- Used internally for rate change coordination

**Usage**: Internal rate change synchronization.

```c
GstEvent *event = gst_event_new_instant_rate_sync_time(
    2.0, running_time, upstream_running_time);
```

## Custom Events

Custom events allow elements to communicate application-specific information.

### GST_EVENT_CUSTOM_UPSTREAM (270)

**Flags**: `UPSTREAM`

Custom event traveling upstream.

```c
GstStructure *s = gst_structure_new("my-custom-event",
    "field", G_TYPE_INT, 42, NULL);
GstEvent *event = gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, s);
```

### GST_EVENT_CUSTOM_DOWNSTREAM (280)

**Flags**: `DOWNSTREAM | SERIALIZED`

Custom event traveling downstream, serialized with data.

### GST_EVENT_CUSTOM_DOWNSTREAM_OOB (290)

**Flags**: `DOWNSTREAM`

Custom out-of-band downstream event (not serialized).

### GST_EVENT_CUSTOM_DOWNSTREAM_STICKY (300)

**Flags**: `DOWNSTREAM | SERIALIZED | STICKY | STICKY_MULTI`

Custom sticky downstream event.

### GST_EVENT_CUSTOM_BOTH (310)

**Flags**: `BOTH | SERIALIZED`

Custom bidirectional event, serialized when downstream.

### GST_EVENT_CUSTOM_BOTH_OOB (320)

**Flags**: `BOTH`

Custom bidirectional out-of-band event.

## Event Flow Diagram

```
SOURCE --> [downstream events] --> SINK
       <-- [upstream events]   <--

Typical downstream flow:
STREAM_START -> CAPS -> SEGMENT -> [BUFFERS + EVENTS] -> EOS

Typical upstream flow (seek):
SEEK -> [pipeline processing] -> FLUSH_START -> FLUSH_STOP -> 
     -> STREAM_START -> CAPS -> SEGMENT -> [BUFFERS]
```

## Best Practices

1. **Always send STREAM_START first** in a new stream
2. **Follow with CAPS** before any buffers
3. **Send SEGMENT** to define timing information
4. **Use sticky events** for state that must persist
5. **Check event handling return values** to detect errors
6. **Implement proper event handlers** in custom elements
7. **Respect event serialization** for data flow synchronization

## See Also

- [GStreamer Event Design Documentation](https://gstreamer.freedesktop.org/documentation/design/events.html)
- [GStreamer API Reference - GstEvent](https://gstreamer.freedesktop.org/documentation/gstreamer/gstevent.html)
