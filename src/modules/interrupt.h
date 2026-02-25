#ifndef __INTERRUPT_H__
#define __INTERRUPT_H__

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

gboolean check_for_interrupt(gpointer user_data);

void _intr_setup(void);

#ifdef __cplusplus
}
#endif

#endif
