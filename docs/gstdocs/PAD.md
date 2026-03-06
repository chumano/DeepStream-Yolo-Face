# GStreamer Pads

## Overview

Pads are the input and output interfaces of an `GstElement`. They are used to negotiate formats and capabilities (Caps) and to transfer data (Buffers and Events) between elements in a pipeline.

## Pad Direction

A pad is defined by its direction:
*   **Source Pad (`GST_PAD_SRC`)**: Produces data. It pushes data out to a peer.
*   **Sink Pad (`GST_PAD_SINK`)**: Consumes data. It receives data from a peer.

Data always flows from a Source pad to a Sink pad.

## Pad Availability

Pads are characterized by their availability:
1.  **Always**: The pad is always present on the element (e.g., `sink` on `fakesink`).
2.  **Sometimes**: The pad is created dynamically depending on the stream content (e.g., demuxers creating pads when a stream is detected).
3.  **Request**: The pad is created on demand by the application (e.g., `tee` element requesting new source pads).

## Data Flow

There are two modes of data transfer:

### 1. Push Mode
The upstream element (source) initiates the data transfer. It calls `gst_pad_push()` on its source pad, which calls the chain function of the peer sink pad.
*   **Chain Function**: `gst_pad_set_chain_function()`. This is the most common method for processing data.

### 2. Pull Mode
The downstream element (sink) initiates the data transfer. It calls `gst_pad_pull_range()` on its sink pad to request a specific range of data from the upstream source pad.
*   **GetRange Function**: `gst_pad_set_getrange_function()`. Used by elements that support random access (e.g., file sources).

## Capabilities (Caps) Negotiation

Before data transfer can begin, pads must agree on the data format. This process is called Caps Negotiation.
1.  **Fixed Caps**: The pad has only one format it can handle.
2.  **Template Caps**: A description of all possible formats a pad can handle.
3.  **Negotiation**: The process involves `query_caps`, `accept_caps`, and `fixate_caps` to narrow down the possibilities to a single structure.

## Pad Probes

Probes allow applications or other elements to monitor or manipulate data flowing through a pad without modifying the element itself.
*   **Buffer Probes**: Inspect or modify buffers.
*   **Event Probes**: Inspect or modify events.
*   **Query Probes**: Intercept queries.
*   **Block/Idle Probes**: Used to block data flow for dynamic pipeline manipulation.

## Pad Linkage

Pads are linked using `gst_pad_link()`. A link is only successful if:
1.  The directions are compatible (Src -> Sink).
2.  The pads are not already linked.
3.  The pads belong to elements in the same bin (usually).
4.  The caps are compatible (though actual negotiation happens later).

## Internal State

A pad maintains internal state flags:
*   `GST_PAD_FLUSHING`: The pad is flushing; no data is accepted.
*   `GST_PAD_EOS`: The pad has received the End-Of-Stream event.
