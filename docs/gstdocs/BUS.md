# GStreamer Bus Documentation

## Overview

The **GstBus** is a fundamental component in GStreamer responsible for delivering messages from streaming threads to the application in a thread-safe, first-in-first-out (FIFO) manner. It acts as a message broker between pipeline elements and the application.

## Core Concepts

### What is a Bus?

A bus is an asynchronous message delivery system that:
- Receives messages from pipeline elements (running in streaming threads)
- Marshals messages between different threads
- Delivers messages to the application (typically in the main thread)
- Maintains message ordering and integrity

### Why Do We Need a Bus?

1. **Thread Safety**: Media streaming happens in separate threads from the application
2. **Asynchronous Communication**: Elements need to notify the application of events without blocking
3. **Centralized Message Handling**: Single point for all pipeline communications
4. **Decoupling**: Separates pipeline logic from application logic

## Architecture

### Key Components

```
┌─────────────────────────────────────────────────────────┐
│                     GstBus                              │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  ┌──────────────┐      ┌──────────────┐               │
│  │ Sync Handler │      │ Message Queue│               │
│  └──────────────┘      └──────────────┘               │
│         │                      │                       │
│         │                      ▼                       │
│         │              ┌──────────────┐               │
│         │              │   GstPoll    │               │
│         │              └──────────────┘               │
│         │                      │                       │
│         └──────────┬───────────┘                       │
│                    ▼                                   │
│            ┌──────────────┐                           │
│            │   GSource    │                           │
│            └──────────────┘                           │
│                    │                                   │
└────────────────────┼───────────────────────────────────┘
                     ▼
              ┌─────────────┐
              │  GMainLoop  │
              └─────────────┘
```

### Internal Structure

#### 1. **GstBusPrivate**
```c
struct _GstBusPrivate {
    GMutex queue_lock;              // Protects message queue
    GstVecDeque *queue;             // FIFO message queue
    SyncHandler *sync_handler;      // Synchronous message handler
    guint num_signal_watchers;      // Number of async signal watches
    guint num_sync_message_emitters;// Number of sync-message emitters
    GSource *gsource;               // GLib event source
    gboolean enable_async;          // Enable async delivery
    GstPoll *poll;                  // File descriptor polling
    GPollFD pollfd;                 // Poll file descriptor
}
```

#### 2. **SyncHandler**
```c
typedef struct {
    GstBusSyncHandler handler;      // Sync handler function
    gpointer user_data;             // User data
    GDestroyNotify destroy_notify;  // Cleanup callback
    gint ref_count;                 // Reference count
} SyncHandler;
```

## Message Flow

### 1. Message Posting (`gst_bus_post`)

```
Element Thread                    Bus                     Application Thread
     │                            │                              │
     │──── gst_bus_post() ───────►│                              │
     │                            │                              │
     │                            │ [Sync Handler?]              │
     │                            │      YES ──────────────────►│
     │                            │       │                      │
     │                            │      NO                      │
     │                            │       │                      │
     │                            │       ▼                      │
     │                            │ [Emit sync-message?]         │
     │                            │      YES ──────────────────►│
     │                            │       │                      │
     │                            │      NO                      │
     │                            │       │                      │
     │                            │       ▼                      │
     │                            │ [Queue Message]              │
     │                            │       │                      │
     │                            │ [Signal GstPoll]             │
     │                            │       │                      │
     │◄─────── return ────────────│       │                      │
     │                            │       │                      │
     │                            │       ▼                      │
     │                            │ [GSource dispatch]───────────►│
     │                            │                              │
```

### 2. Message Retrieval

#### Synchronous Retrieval (`gst_bus_pop`, `gst_bus_peek`)
- **Immediate**: Returns immediately with or without a message
- **Thread-safe**: Can be called from any thread
- **No blocking**: Returns NULL if queue is empty

#### Timed Retrieval (`gst_bus_timed_pop_filtered`)
- **Blocking**: Waits up to specified timeout
- **Filtering**: Only returns messages matching type mask
- **Efficient**: Uses GstPoll for efficient waiting

#### Asynchronous Retrieval (Watch/Signal)
- **Event-driven**: Integrates with GMainLoop
- **Automatic**: Messages delivered via callbacks/signals
- **Non-blocking**: Application remains responsive

## Message Handling Modes

### 1. Synchronous Handler

```c
GstBusSyncReply sync_handler(GstBus *bus, GstMessage *message, gpointer data) {
    // Called from posting thread
    // Handle message immediately
    return GST_BUS_PASS;  // or GST_BUS_DROP, GST_BUS_ASYNC
}

gst_bus_set_sync_handler(bus, sync_handler, user_data, notify);
```

**Characteristics:**
- Runs in the posting thread
- Blocks the posting thread
- Can drop, pass, or request async delivery
- No marshalling overhead

**Return Values:**
- `GST_BUS_DROP`: Message handled, don't queue
- `GST_BUS_PASS`: Queue message for async delivery
- `GST_BUS_ASYNC`: Queue and wait for delivery

### 2. Asynchronous Watch

```c
gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data) {
    // Called from main loop
    // Handle message asynchronously
    return TRUE;  // Keep watch
}

guint watch_id = gst_bus_add_watch(bus, bus_callback, user_data);
```

**Characteristics:**
- Runs in GMainLoop thread
- Non-blocking for posting thread
- Messages marshalled via GSource
- Integrates with GLib event system

### 3. Signal-based

```c
void on_message(GstBus *bus, GstMessage *message, gpointer data) {
    // Handle specific message type
}

gst_bus_add_signal_watch(bus);
g_signal_connect(bus, "message::error", G_CALLBACK(on_message), NULL);
```

**Characteristics:**
- GLib signal emission
- Can connect to specific message types
- Multiple handlers possible
- Detail quark for message type filtering

### 4. Polling

```c
GstMessage *message = gst_bus_poll(bus, GST_MESSAGE_ANY, timeout);
```

**Characteristics:**
- Blocks until message arrives or timeout
- Uses internal GMainLoop
- **WARNING**: Can cause re-entrancy issues
- Generally discouraged for complex applications

## Threading Model

### Thread Safety Guarantees

1. **Message Queue**: Protected by `queue_lock` mutex
2. **Sync Handler**: Atomic reference counting
3. **GSource**: Protected by GLib's main context lock
4. **Message Ownership**: Clear transfer semantics

### Lock-Free Operations

- Message reference counting (atomic)
- Sync handler reference counting (atomic)
- Flag checks (atomic reads)

### Lock Hierarchy

```
GST_OBJECT_LOCK (bus)
    └── queue_lock
        └── GMainContext lock (implicit in GSource operations)
```

## Message Queue Management

### Queue Implementation

- **Type**: `GstVecDeque` (vector-based deque)
- **Initial Capacity**: 32 messages
- **Growth**: Dynamic, no hard limit
- **Warning Threshold**: Every 1024 messages

### Queue Operations

```c
// Push to tail (from posting thread)
gst_vec_deque_push_tail(queue, message);
gst_poll_write_control(poll);  // Wake up waiting threads

// Pop from head (from application thread)
message = gst_vec_deque_pop_head(queue);
gst_poll_read_control(poll);   // Clear wake signal
```

### Overflow Handling

When queue length exceeds multiples of 1024:
```
WARNING: queue overflows with N messages. Application is too slow 
or is not handling messages. Please add a message handler, otherwise 
the queue will grow infinitely.
```

## GSource Integration

### GSource Creation

```c
GSource *source = gst_bus_create_watch(bus);
g_source_set_callback(source, callback, user_data, notify);
g_source_attach(source, context);
```

### GSource Lifecycle

```
Creation → Attachment → Dispatch → Destruction
    │          │           │            │
    │          │           │            └── gst_bus_source_dispose()
    │          │           │                gst_bus_source_finalize()
    │          │           │
    │          │           └── gst_bus_source_dispatch()
    │          │               - Pop message
    │          │               - Call callback
    │          │               - Return keep status
    │          │
    │          └── g_source_attach()
    │              - Add to main context
    │              - Register pollfd
    │
    └── gst_bus_create_watch_unlocked()
        - Allocate GstBusSource
        - Set up pollfd monitoring
```

### GSource Functions

```c
static GSourceFuncs gst_bus_source_funcs = {
    NULL,                        // prepare
    gst_bus_source_check,        // check if ready
    gst_bus_source_dispatch,     // dispatch callback
    gst_bus_source_finalize      // cleanup
};
```

**Check Function:**
- Tests if POLLIN/HUP/ERR on pollfd
- Returns TRUE if messages available

**Dispatch Function:**
- Pops message from queue
- Calls user callback
- Returns callback result (TRUE = keep, FALSE = remove)

## Bus States

### Flushing State

```c
gst_bus_set_flushing(bus, TRUE);  // Enter flushing
gst_bus_set_flushing(bus, FALSE); // Exit flushing
```

**When Flushing:**
- All `gst_bus_post()` calls fail
- Existing messages are unreffed and discarded
- New messages are rejected
- Typically set when pipeline goes NULL → READY

**Implementation:**
```c
if (GST_OBJECT_FLAG_IS_SET(bus, GST_BUS_FLUSHING)) {
    gst_message_unref(message);
    return FALSE;
}
```

### Enable Async Property

```c
g_object_new(GST_TYPE_BUS, "enable-async", TRUE, NULL);
```

**When FALSE:**
- No message queue created
- No GstPoll created
- Only sync handlers work
- Used for child element buses in bins

## Common Patterns

### Pattern 1: Simple Async Watch

```c
static gboolean
bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
            // Handle error
            return FALSE;  // Stop watch
        case GST_MESSAGE_EOS:
            // Handle end-of-stream
            return FALSE;
        default:
            break;
    }
    return TRUE;  // Continue watching
}

// Setup
GstBus *bus = gst_pipeline_get_bus(pipeline);
gst_bus_add_watch(bus, bus_callback, user_data);
gst_object_unref(bus);

// Cleanup (automatic when callback returns FALSE, or:)
gst_bus_remove_watch(bus);
```

### Pattern 2: Sync Handler for Performance

```c
static GstBusSyncReply
sync_handler(GstBus *bus, GstMessage *msg, gpointer data) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_QOS) {
        // Handle QoS immediately, don't queue
        handle_qos(msg);
        return GST_BUS_DROP;
    }
    // Queue other messages
    return GST_BUS_PASS;
}

gst_bus_set_sync_handler(bus, sync_handler, data, notify);
```

### Pattern 3: Signal Watch with Type Filtering

```c
static void
on_error(GstBus *bus, GstMessage *msg, gpointer data) {
    // Only called for ERROR messages
}

gst_bus_add_signal_watch(bus);
g_signal_connect(bus, "message::error", G_CALLBACK(on_error), NULL);

// Cleanup
gst_bus_remove_signal_watch(bus);
```

### Pattern 4: Polling with Timeout

```c
// Wait up to 5 seconds for EOS or ERROR
GstMessage *msg = gst_bus_timed_pop_filtered(bus,
    5 * GST_SECOND,
    GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

if (msg) {
    // Handle message
    gst_message_unref(msg);
} else {
    // Timeout
}
```

## Performance Considerations

### Optimization Tips

1. **Use Sync Handler for Time-Critical Messages**
   - Avoids queuing overhead
   - Processes in posting thread
   - Example: QoS, latency messages

2. **Filter Message Types**
   - Reduces queue size
   - Less memory usage
   - Faster dispatch

3. **Avoid `gst_bus_poll()`**
   - Causes re-entrancy
   - Inefficient for long timeouts
   - Use async watches instead

4. **Process Messages Promptly**
   - Prevents queue growth
   - Reduces memory pressure
   - Improves responsiveness

### Memory Management

**Message Ownership:**
- `gst_bus_post()`: Takes ownership (transfer full)
- `gst_bus_pop()`: Returns ownership (transfer full)
- `gst_bus_peek()`: Returns reference (transfer full, must unref)
- Callbacks: Message freed after callback returns

**Automatic Cleanup:**
- Messages unreffed when bus disposed
- Sync handler cleanup via destroy_notify
- Watch cleanup via GSource destroy function

## Debugging

### Enable Debug Output

```bash
GST_DEBUG=GST_BUS:5 ./myapp
```

### Common Debug Messages

- `"created new bus"` - Bus instantiated
- `"posting on bus %p"` - Message being posted
- `"pushing on async queue"` - Message queued
- `"got message %p, %s from %s"` - Message retrieved
- `"discarding message, does not match mask"` - Filtered out
- `"bus is flushing"` - Post rejected due to flushing

### Debugging Tools

```c
// Check for pending messages
gboolean pending = gst_bus_have_pending(bus);

// Peek without removing
GstMessage *msg = gst_bus_peek(bus);
if (msg) {
    g_print("Next message: %s\n", GST_MESSAGE_TYPE_NAME(msg));
    gst_message_unref(msg);
}
```

## Best Practices

1. **Always Set Up Message Handling**
   - Prevents queue overflow
   - Catches errors and warnings
   - Handles state changes properly

2. **Match Add/Remove Calls**
   - `gst_bus_add_signal_watch()` ↔ `gst_bus_remove_signal_watch()`
   - `gst_bus_enable_sync_message_emission()` ↔ `gst_bus_disable_sync_message_emission()`

3. **Handle All Critical Message Types**
   - `GST_MESSAGE_ERROR`: Fatal errors
   - `GST_MESSAGE_EOS`: End of stream
   - `GST_MESSAGE_STATE_CHANGED`: Pipeline state
   - `GST_MESSAGE_WARNING`: Non-fatal issues

4. **Unref Messages After Use**
   ```c
   GstMessage *msg = gst_bus_pop(bus);
   if (msg) {
       // Process message
       gst_message_unref(msg);  // Don't forget!
   }
   ```

5. **Use Appropriate Method for Your Use Case**
   - GUI apps: Async watch
   - Simple scripts: Polling with timeout
   - Performance-critical: Sync handler
   - Event-driven: Signal watch

## Related Components

- **GstMessage**: The actual message data
- **GstPipeline**: Has one bus instance
- **GstElement**: Posts messages to bus
- **GMainLoop**: Integrates with async watches
- **GstPoll**: Efficient polling mechanism

## References

- `gst/gstbus.h` - Public API declarations
- `gst/gstbus.c` - Implementation
- `gst/gstmessage.h` - Message types and structures
- GStreamer Application Development Manual - Bus section
