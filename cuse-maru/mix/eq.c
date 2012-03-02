#include "eq.h"
#include <math.h>
#include <complex.h>
#include <stdlib.h>

struct maru_eq
{
   complex double *butterfly_l;
   complex double *butterfly_r;

   size_t samples;

   size_t range_lo[2];
   size_t range_mid[2];
   size_t range_hi[2];

   double gain_lo;
   double gain_mid;
   double gain_hi;

   unsigned bitrange;
};

static unsigned bitrange(unsigned len)
{
   unsigned ret = 0;
   while ((len >>= 1))
      ret++;

   return ret;
}

maru_eq_t *maru_eq_new(size_t samples)
{
   maru_eq_t *eq = calloc(1, sizeof(*eq));
   if (!eq)
      goto error;

   eq->butterfly_l = calloc(sizeof(complex double), samples);
   eq->butterfly_r = calloc(sizeof(complex double), samples);
   if (!eq->butterfly_l || !eq->butterfly_r)
      goto error;

   eq->samples = samples;

   unsigned chop_lo = samples / 16;
   unsigned chop_hi = samples / 4;
   eq->range_lo[0] = 1;
   eq->range_lo[1] = chop_lo;
   eq->range_mid[0] = chop_lo;
   eq->range_mid[1] = chop_hi;
   eq->range_hi[0] = chop_hi;
   eq->range_hi[1] = samples / 2;

   eq->gain_lo = 0.4;
   eq->gain_mid = 0.0;
   eq->gain_hi = 0.2;

   return eq;

error:
   maru_eq_free(eq);
   return NULL;
}

static unsigned bitswap(unsigned i, unsigned range)
{
   unsigned ret = 0;
   for (unsigned shifts = 0; shifts < range; shifts++)
      ret |= i & (1 << (range - shifts - 1)) ? (1 << shifts) : 0;

   return ret;
}

// When interleaving the butterfly buffer, addressing puts bits in reverse.
// [0, 1, 2, 3, 4, 5, 6, 7] => [0, 4, 2, 6, 1, 5, 3, 7] 
static void interleave(complex double *butterfly_buf, size_t samples)
{
   unsigned range = bitrange(samples);
   for (unsigned i = 0; i < samples; i++)
   {
      unsigned target = bitswap(i, range);
      if (target > i)
      {
         complex double tmp = butterfly_buf[target];
         butterfly_buf[target] = butterfly_buf[i];
         butterfly_buf[i] = tmp;
      }
   }
}

static complex double gen_phase(double index)
{
   return cexp(M_PI * I * index);
}

static void butterfly(complex double *a, complex double *b, complex double mod)
{
   mod *= *b;
   complex double a_ = *a + mod;
   complex double b_ = *a - mod;
   *a = a_;
   *b = b_;
}

static void butterflies(complex double *butterfly_buf, double phase_dir, size_t step_size, size_t samples)
{
   for (unsigned i = 0; i < samples; i += 2 * step_size)
      for (unsigned j = i; j < i + step_size; j++)
         butterfly(&butterfly_buf[j], &butterfly_buf[j + step_size], gen_phase((phase_dir * (j - i)) / step_size));
}

static void apply_gain_range(maru_eq_t *eq,
      unsigned begin, unsigned end,
      double gain)
{
   for (unsigned i = begin; i < end; i++)
   {
      eq->butterfly_l[i] *= gain;
      eq->butterfly_r[i] *= gain;
      eq->butterfly_l[eq->samples - i] *= gain;
      eq->butterfly_r[eq->samples - i] *= gain;
   }
}

static void apply_gain(maru_eq_t *eq)
{
   apply_gain_range(eq,
         eq->range_lo[0], eq->range_lo[1], eq->gain_lo);
   apply_gain_range(eq,
         eq->range_mid[0], eq->range_mid[1], eq->gain_mid);
   apply_gain_range(eq,
         eq->range_hi[0], eq->range_hi[1], eq->gain_hi);

   //eq->butterfly_l[0] *= eq->gain_lo;
   //eq->butterfly_r[0] *= eq->gain_lo;

   eq->butterfly_l[eq->samples / 2] *= eq->gain_hi;
   eq->butterfly_r[eq->samples / 2] *= eq->gain_hi;
}

static void calculate_fft(maru_eq_t *eq, const float *data)
{
   for (size_t i = 0; i < eq->samples; i++)
   {
      eq->butterfly_l[i] = data[2 * i + 0];
      eq->butterfly_r[i] = data[2 * i + 1];
   }

   // Interleave buffer to work with FFT.
   interleave(eq->butterfly_l, eq->samples);
   interleave(eq->butterfly_r, eq->samples);

   // Fly, lovely butterflies! :D
   for (unsigned step_size = 1; step_size < eq->samples; step_size *= 2)
   {
      butterflies(eq->butterfly_l, -1.0, step_size, eq->samples);
      butterflies(eq->butterfly_r, -1.0, step_size, eq->samples);
   }
}

static void calculate_ifft(maru_eq_t *eq, float *data)
{
   interleave(eq->butterfly_l, eq->samples);
   interleave(eq->butterfly_r, eq->samples);

   for (unsigned step_size = 1; step_size < eq->samples; step_size *= 2)
   {
      butterflies(eq->butterfly_l, 1.0, step_size, eq->samples);
      butterflies(eq->butterfly_r, 1.0, step_size, eq->samples);
   }

   for (size_t i = 0; i < eq->samples; i++)
   {
      data[2 * i + 0] = creal(eq->butterfly_l[i]) / eq->samples;
      data[2 * i + 1] = creal(eq->butterfly_r[i]) / eq->samples;
   }
}

void maru_eq_process(maru_eq_t *eq, float *data, size_t frames)
{
   for (size_t i = 0; i < frames; i += eq->samples)
   {
      calculate_fft(eq, data + 2 * i);
      apply_gain(eq);
      calculate_ifft(eq, data + 2 * i);
   }
}

void maru_eq_free(maru_eq_t *eq)
{
   if (eq)
   {
      free(eq->butterfly_l);
      free(eq->butterfly_r);
      free(eq);
   }
}
