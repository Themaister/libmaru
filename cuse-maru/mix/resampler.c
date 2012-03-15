/*  cuse-maru - CUSE implementation of Open Sound System using libmaru.
 *  Copyright (C) 2012 - Hans-Kristian Arntzen
 *  Copyright (C) 2012 - Agnes Heyer
 *
 *  cuse-maru is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  cuse-maru is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with cuse-maru.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "resampler.h"
#include "utils.h"

#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <malloc.h>

#if __SSE__
#include <xmmintrin.h>
#endif

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

#define SIDELOBES 32
#define TAPS (SIDELOBES * 2)
#define CUTOFF 0.9

#define PHASE_INDEX 0
#define DELTA_INDEX 1

struct maru_resampler
{
   float phase_table[PHASES][2][2 * SIDELOBES];
   float buffer_l[2 * TAPS];
   float buffer_r[2 * TAPS];

   unsigned ptr;

   uint32_t ratio;
   uint32_t time;

   maru_fifo *fifo;
};

static inline double sinc(double val)
{
   if (fabs(val) < 0.00001)
      return 1.0;
   else
      return sin(val) / val;
}

static inline double lanzcos(double phase)
{
   return sinc(phase);
}

static void init_sinc_table(struct maru_resampler *resamp)
{
   // Sinc phases: [..., p + 3, p + 2, p + 1, p + 0, p - 1, p - 2, p - 3, p - 4, ...]
   for (int i = 0; i < PHASES; i++)
   {
      for (int j = 0; j < 2 * SIDELOBES; j++)
      {
         double p = (double)i / PHASES;
         double sinc_phase = M_PI * (p + (SIDELOBES - 1 - j));
         resamp->phase_table[i][PHASE_INDEX][j] = CUTOFF * sinc(CUTOFF * sinc_phase) * lanzcos(sinc_phase / SIDELOBES);
      }
   }

   // Optimize linear interpolation.
   for (int i = 0; i < PHASES - 1; i++)
   {
      for (int j = 0; j < 2 * SIDELOBES; j++)
      {
         resamp->phase_table[i][DELTA_INDEX][j] =
            (resamp->phase_table[i + 1][PHASE_INDEX][j] - resamp->phase_table[i][PHASE_INDEX][j]) / SUBPHASES;
      }
   }

   // Interpolation between [PHASES - 1] => [PHASES] 
   for (int j = 0; j < TAPS; j++)
   {
      double p = 1.0;
      double sinc_phase = M_PI * (p + (SIDELOBES - 1 - j));
      double phase = CUTOFF * sinc(CUTOFF * sinc_phase) * lanzcos(sinc_phase / SIDELOBES);

      float result = (phase - resamp->phase_table[PHASES - 1][PHASE_INDEX][j]) / SUBPHASES;
      resamp->phase_table[PHASES - 1][DELTA_INDEX][j] = result;
   }
}

#if __SSE__
static void process_sinc(struct maru_resampler *resamp, float * restrict out_buffer)
{
   __m128 sum_l = _mm_setzero_ps();
   __m128 sum_r = _mm_setzero_ps();

   const float *buffer_l = resamp->buffer_l + resamp->ptr;
   const float *buffer_r = resamp->buffer_r + resamp->ptr;

   unsigned phase = resamp->time >> PHASES_SHIFT;
   unsigned delta = (resamp->time >> SUBPHASES_SHIFT) & SUBPHASES_MASK;
   __m128 delta_f = _mm_set1_ps(delta);

   const float *phase_table = resamp->phase_table[phase][PHASE_INDEX];
   const float *delta_table = resamp->phase_table[phase][DELTA_INDEX];

   for (unsigned i = 0; i < TAPS; i += 4)
   {
      __m128 buf_l  = _mm_loadu_ps(buffer_l + i);
      __m128 buf_r  = _mm_loadu_ps(buffer_r + i);

      __m128 phases = _mm_load_ps(phase_table + i);
      __m128 deltas = _mm_load_ps(delta_table + i);

      __m128 sinc   = _mm_add_ps(phases, _mm_mul_ps(deltas, delta_f));

      sum_l         = _mm_add_ps(sum_l, _mm_mul_ps(buf_l, sinc));
      sum_r         = _mm_add_ps(sum_r, _mm_mul_ps(buf_r, sinc));
   }

   // Them annoying shuffles :V
   // sum_l = { l3, l2, l1, l0 }
   // sum_r = { r3, r2, r1, r0 }

   __m128 sum = _mm_add_ps(_mm_shuffle_ps(sum_l, sum_r, _MM_SHUFFLE(1, 0, 1, 0)),
         _mm_shuffle_ps(sum_l, sum_r, _MM_SHUFFLE(3, 2, 3, 2)));

   // sum   = { r1, r0, l1, l0 } + { r3, r2, l3, l2 }
   // sum   = { R1, R0, L1, L0 }

   sum = _mm_add_ps(_mm_shuffle_ps(sum, sum, _MM_SHUFFLE(3, 3, 1, 1)), sum);

   // sum   = {R1, R1, L1, L1 } + { R1, R0, L1, L0 }
   // sum   = { X,  R,  X,  L } 

   // Store L
   _mm_store_ss(out_buffer + 0, sum);

   // movehl { X, R, X, L } == { X, R, X, R }
   _mm_store_ss(out_buffer + 1, _mm_movehl_ps(sum, sum));
}
#else // Plain ol' C99
static void process_sinc(struct maru_resampler *resamp, float * restrict out_buffer)
{
   float sum_l = 0.0f;
   float sum_r = 0.0f;
   const float *buffer_l = resamp->buffer_l + resamp->ptr;
   const float *buffer_r = resamp->buffer_r + resamp->ptr;

   unsigned phase = resamp->time >> PHASES_SHIFT;
   unsigned delta = (resamp->time >> SUBPHASES_SHIFT) & SUBPHASES_MASK;
   float delta_f = (float)delta;

   const float *phase_table = resamp->phase_table[phase][PHASE_INDEX];
   const float *delta_table = resamp->phase_table[phase][DELTA_INDEX];

   for (unsigned i = 0; i < TAPS; i++)
   {
      float sinc_val = phase_table[i] + delta_f * delta_table[i];
      sum_l         += buffer_l[i] * sinc_val;
      sum_r         += buffer_r[i] * sinc_val;
   }

   out_buffer[0] = sum_l;
   out_buffer[1] = sum_r;
}
#endif

size_t resampler_process(struct maru_resampler *resamp, float *data, size_t frames)
{
   float conv_buf[2 * frames];
   const float *input = conv_buf;

   uint32_t needed_input_frames = ((uint64_t)resamp->time + (uint64_t)resamp->ratio * frames) >> FRAMES_SHIFT;
   size_t needed_size = needed_input_frames * 2 * sizeof(int16_t); // Hardcode for stereo 16-bit.

   size_t avail = maru_fifo_read_avail(resamp->fifo);
   if (avail > needed_size)
      avail = needed_size;

   struct maru_fifo_locked_region region;
   maru_fifo_read_lock(resamp->fifo, avail, &region);

   audio_convert_s16_to_float(conv_buf,
         region.first,
         region.first_size / sizeof(int16_t));

   if (region.second)
   {
      audio_convert_s16_to_float(conv_buf + region.first_size / sizeof(int16_t),
            region.second,
            region.second_size / sizeof(int16_t));
   }

   maru_fifo_read_unlock(resamp->fifo, &region);

   memset(conv_buf + avail / (2 * sizeof(int16_t)), 0, needed_size - avail);

   while (frames)
   {
      process_sinc(resamp, data);
      data += 2;
      frames--;

      // Shuffle in new data.
      resamp->time += resamp->ratio;
      if (resamp->time >= PHASES_WRAP)
      {
         resamp->buffer_l[resamp->ptr + TAPS] = resamp->buffer_l[resamp->ptr] = *input++;
         resamp->buffer_r[resamp->ptr + TAPS] = resamp->buffer_r[resamp->ptr] = *input++;
         resamp->ptr = (resamp->ptr + 1) & (TAPS - 1);

         resamp->time -= PHASES_WRAP;
      }
   }

   return avail;
}

maru_resampler_t *resampler_init(maru_fifo *fifo,
      unsigned in_rate, unsigned out_rate)
{
   maru_resampler_t *resamp = memalign(16, sizeof(*resamp));
   if (!resamp)
      return NULL;

   memset(resamp, 0, sizeof(*resamp));

   resamp->ratio = ((uint64_t)PHASES_WRAP * in_rate) / out_rate;
   resamp->fifo = fifo;
   resamp->time = 0;
   resamp->ptr = 0;
   init_sinc_table(resamp);
   memset(resamp->buffer_l, 0, sizeof(resamp->buffer_l));
   memset(resamp->buffer_r, 0, sizeof(resamp->buffer_r));

   return resamp;
}

void resampler_free(maru_resampler_t *resamp)
{
   free(resamp);
}

