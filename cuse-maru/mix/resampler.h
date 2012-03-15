#ifndef RESAMPLER_H__
#define RESAMPLER_H__

#include <libmaru/fifo.h>

typedef struct maru_resampler maru_resampler_t;

maru_resampler_t *resampler_init(maru_fifo *fifo,
      unsigned in_rate, unsigned out_rate);

void resampler_free(maru_resampler_t *resamp);

size_t resampler_process(struct maru_resampler *resamp,
      float *data,
      size_t frames);

#endif

