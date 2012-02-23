#include "resampler.h"
#include "utils.h"

#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#if __SSE__
#include <xmmintrin.h>
#endif

static inline double sinc(double val)
{
   if (fabs(val) < 0.00001)
      return 1.0;
   else
      return sin(val) / val;
}

static void init_sinc_table(struct maru_resampler *resamp)
{
   for (unsigned i = 0; i <= PHASES; i++)
   {
      for (unsigned j = 0; j < SIDELOBES; j++)
      {
         double sinc_phase = M_PI * ((double)i / PHASES + (double)j);
         resamp->phase_table[i][j] = sinc(sinc_phase) * sinc(sinc_phase / SIDELOBES); // Lanczos window.
      }
   }

   // Optimize linear interpolation.
   for (unsigned i = 0; i < PHASES; i++)
      for (unsigned j = 0; j < SIDELOBES; j++)
         resamp->delta_table[i][j] = resamp->phase_table[i + 1][j] - resamp->phase_table[i][j];
}

#if __SSE__
static void process_sinc(struct maru_resampler *resamp, float * restrict out_buffer)
{
   __m128 sum_l = _mm_setzero_ps();
   __m128 sum_r = _mm_setzero_ps();

   const float *buffer_l = resamp->buffer_l;
   const float *buffer_r = resamp->buffer_r;

   unsigned phase = resamp->time >> PHASES_SHIFT;
   unsigned delta = (resamp->time >> SUBPHASES_SHIFT) & SUBPHASES_MASK;
   __m128 delta_f = _mm_set1_ps((float)delta / SUBPHASES);

   const float *phase_table = resamp->phase_table[phase];
   const float *delta_table = resamp->delta_table[phase];

   __m128 sinc_vals[SIDELOBES / 4];
   for (unsigned i = 0; i < SIDELOBES; i += 4)
   {
      __m128 phases    = _mm_load_ps(phase_table + i);
      __m128 deltas    = _mm_load_ps(delta_table + i);
      sinc_vals[i / 4] = _mm_add_ps(phases, _mm_mul_ps(deltas, delta_f));
   }

   // Older data.
   for (unsigned i = 0; i < SIDELOBES; i += 4)
   {
      __m128 buf_l = _mm_loadr_ps(buffer_l + SIDELOBES - 4 - i);
      sum_l        = _mm_add_ps(sum_l, _mm_mul_ps(buf_l, sinc_vals[i / 4]));

      __m128 buf_r = _mm_loadr_ps(buffer_r + SIDELOBES - 4 - i);
      sum_r        = _mm_add_ps(sum_r, _mm_mul_ps(buf_r, sinc_vals[i / 4]));
   }

   // Newer data.
   unsigned reverse_phase = PHASES_WRAP - resamp->time;
   phase   = reverse_phase >> PHASES_SHIFT;
   delta   = (reverse_phase >> SUBPHASES_SHIFT) & SUBPHASES_MASK;
   delta_f = _mm_set1_ps((float)delta / SUBPHASES);

   phase_table = resamp->phase_table[phase];
   delta_table = resamp->delta_table[phase];

   for (unsigned i = 0; i < SIDELOBES; i += 4)
   {
      __m128 phases    = _mm_load_ps(phase_table + i);
      __m128 deltas    = _mm_load_ps(delta_table + i);
      sinc_vals[i / 4] = _mm_add_ps(phases, _mm_mul_ps(deltas, delta_f));
   }

   for (unsigned i = 0; i < SIDELOBES; i += 4)
   {
      __m128 buf_l = _mm_load_ps(buffer_l + SIDELOBES + i);
      sum_l        = _mm_add_ps(sum_l, _mm_mul_ps(buf_l, sinc_vals[i / 4]));

      __m128 buf_r = _mm_load_ps(buffer_r + SIDELOBES + i);
      sum_r        = _mm_add_ps(sum_r, _mm_mul_ps(buf_r, sinc_vals[i / 4]));
   }

   __m128 sum_shuf_l = _mm_shuffle_ps(sum_l, sum_r, _MM_SHUFFLE(1, 0, 1, 0));
   __m128 sum_shuf_r = _mm_shuffle_ps(sum_l, sum_r, _MM_SHUFFLE(3, 2, 3, 2));
   __m128 sum        = _mm_add_ps(sum_shuf_l, sum_shuf_r);

   union
   {
      float f[4];
      __m128 v;
   } u;
   u.v = sum;

   out_buffer[0] = u.f[0] + u.f[1];
   out_buffer[1] = u.f[2] + u.f[3];
}
#else // Plain ol' C99
static void process_sinc(struct maru_resampler *resamp, float * restrict out_buffer)
{
   float sum_l = 0.0f;
   float sum_r = 0.0f;
   const float *buffer_l = resamp->buffer_l;
   const float *buffer_r = resamp->buffer_r;

   unsigned phase = resamp->time >> PHASES_SHIFT;
   unsigned delta = (resamp->time >> SUBPHASES_SHIFT) & SUBPHASES_MASK;
   float delta_f = (float)delta / SUBPHASES;

   const float *phase_table = resamp->phase_table[phase];
   const float *delta_table = resamp->delta_table[phase];

   float sinc_vals[SIDELOBES];
   for (unsigned i = 0; i < SIDELOBES; i++)
      sinc_vals[i] = phase_table[i] + delta_f * delta_table[i];

   // Older data.
   for (unsigned i = 0; i < SIDELOBES; i++)
   {
      sum_l += buffer_l[SIDELOBES - 1 - i] * sinc_vals[i];
      sum_r += buffer_r[SIDELOBES - 1 - i] * sinc_vals[i];
   }

   // Newer data.
   unsigned reverse_phase = PHASES_WRAP - resamp->time;
   phase   = reverse_phase >> PHASES_SHIFT;
   delta   = (reverse_phase >> SUBPHASES_SHIFT) & SUBPHASES_MASK;
   delta_f = (float)delta / SUBPHASES;

   phase_table = resamp->phase_table[phase];
   delta_table = resamp->delta_table[phase];

   for (unsigned i = 0; i < SIDELOBES; i++)
      sinc_vals[i] = phase_table[i] + delta_f * delta_table[i];

   for (unsigned i = 0; i < SIDELOBES; i++)
   {
      sum_l += buffer_l[SIDELOBES + i] * sinc_vals[i];
      sum_r += buffer_r[SIDELOBES + i] * sinc_vals[i];
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
         memmove(resamp->buffer_l, resamp->buffer_l + 1,
               sizeof(resamp->buffer_l) - sizeof(float));
         memmove(resamp->buffer_r, resamp->buffer_r + 1,
               sizeof(resamp->buffer_r) - sizeof(float));

         resamp->buffer_l[2 * SIDELOBES - 1] = *input++;
         resamp->buffer_r[2 * SIDELOBES - 1] = *input++;

         resamp->time -= PHASES_WRAP;
      }
   }

   return avail;
}


void resampler_init(struct maru_resampler *resamp,
      maru_fifo *fifo,
      unsigned in_rate, unsigned out_rate)
{
   resamp->ratio = ((uint64_t)PHASES_WRAP * in_rate) / out_rate;
   resamp->fifo = fifo;
   resamp->time = 0;
   init_sinc_table(resamp);
   memset(resamp->buffer_l, 0, sizeof(resamp->buffer_l));
   memset(resamp->buffer_r, 0, sizeof(resamp->buffer_r));
}

