#ifndef RESAMPLER_H__
#define RESAMPLER_H__

#include "../../fifo.h"

#define PHASE_BITS 8
#define SUBPHASE_BITS 16

#define PHASES (1 << PHASE_BITS)
#define PHASES_SHIFT (SUBPHASE_BITS)
#define PHASES_MASK ((PHASES << 1) - 1)
#define SUBPHASES (1 << SUBPHASE_BITS)
#define SUBPHASES_SHIFT 0
#define SUBPHASES_MASK ((1 << SUBPHASE_BITS) - 1)
#define PHASES_WRAP (1 << (PHASE_BITS + SUBPHASE_BITS))
#define FRAMES_SHIFT (PHASE_BITS + SUBPHASE_BITS)

#define SIDELOBES 8
#define TAPS (SIDELOBES * 2)

#define PHASE_INDEX 0
#define DELTA_INDEX 1

typedef struct maru_resampler maru_resampler_t;

maru_resampler_t *resampler_init(maru_fifo *fifo,
      unsigned in_rate, unsigned out_rate);

void resampler_free(maru_resampler_t *resamp);

size_t resampler_process(struct maru_resampler *resamp,
      float *data,
      size_t frames);

#endif

