#ifndef __DEEPSTREAM_H__
#define __DEEPSTREAM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <nvdsgstutils.h>
#include <cuda_runtime_api.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "gstnvdsmeta.h"
#include "nvbufsurface.h"

#include "modules/interrupt.h"
#include "modules/perf.h"
#include "detection_manager.h"

// =============================================================================
// Application Structures
// =============================================================================

// Global detection manager instance
static DetectionManager *detection_manager = NULL;

#ifdef __cplusplus
}
#endif

#endif
