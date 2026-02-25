#ifndef __DEEPSTREAM_H__
#define __DEEPSTREAM_H__

#include <nvdsgstutils.h>
#include <cuda_runtime_api.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "gstnvdsmeta.h"
#include "nvbufsurface.h"
#include "nvbufsurftransform.h"

#include "modules/interrupt.h"
#include "modules/perf.h"
#include "detection_manager.h"


// Global detection manager instance
static DetectionManager *detection_manager = NULL;

#endif
