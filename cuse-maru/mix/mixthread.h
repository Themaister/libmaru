#ifndef MIXTHREAD_H__
#define MIXTHREAD_H__

#include <stdbool.h>

bool start_mix_thread(void);

long src_callback(void *cb_data, float **data);

#endif

