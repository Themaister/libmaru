#ifndef RESAMPLER_H__
#define RESAMPLER_H__

#include "../../fifo.h"

#define ALIGNED __attribute__((aligned(16)))
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

struct maru_resampler
{
   float phase_table[PHASES + 1][SIDELOBES] ALIGNED;
   float delta_table[PHASES + 1][SIDELOBES] ALIGNED;
   float buffer_l[2 * SIDELOBES] ALIGNED;
   float buffer_r[2 * SIDELOBES] ALIGNED;

   uint32_t ratio;
   uint32_t time;

   maru_fifo *fifo;
};

void resampler_init(struct maru_resampler *resamp,
      maru_fifo *fifo,
      unsigned in_rate, unsigned out_rate);

size_t resampler_process(struct maru_resampler *resamp,
      float *data,
      size_t frames);

#endif

