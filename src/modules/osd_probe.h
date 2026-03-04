#ifndef __OSD_PROBE_H__
#define __OSD_PROBE_H__

#include <gst/gst.h>
#include "gstnvdsmeta.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GstPadProbeCallback for the nvdsosd sink pad.
 * Validates the buffer surface, iterates frame/object metadata,
 * applies custom bounding-boxes, draws landmark circles, and adds
 * an NTP timestamp overlay per frame.
 */
GstPadProbeReturn nvosd_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info,
                                              gpointer user_data);

#ifdef __cplusplus
}
#endif

#endif /* __OSD_PROBE_H__ */
