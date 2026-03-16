# Tracing

This subsystem will provide a mechanism to get structured tracing info from GStreamer applications. This can be used for post-run analysis as well as for live introspection.

Use cases
I’d like to get statistics from a running application.

I’d like to to understand which parts of my pipeline use how many resources.

I’d like to know which parts of the pipeline use how much memory.

I’d like to know about ref-counts of parts in the pipeline to find ref-count issues.

https://gstreamer.freedesktop.org/documentation/additional/design/tracing.html?gi-language=c