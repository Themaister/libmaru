#include "utils.h"

#if __SSE2__
#include <emmintrin.h>
#elif __ALTIVEC__
#include <altivec.h>
#endif

void audio_convert_s16_to_float_C(float *out,
      const int16_t *in, size_t samples)
{
   for (size_t i = 0; i < samples; i++)
      out[i] = (float)in[i] / 0x8000; 
}

void audio_convert_float_to_s16_C(int16_t *out,
      const float *in, size_t samples)
{
   for (size_t i = 0; i < samples; i++)
   {
      int32_t val = (int32_t)(in[i] * 0x8000);
      out[i] = (val > 0x7FFF) ? 0x7FFF : (val < -0x8000 ? -0x8000 : (int16_t)val);
   }
}

#if __SSE2__
void audio_convert_s16_to_float_SSE2(float *out,
      const int16_t *in, size_t samples)
{
   __m128 factor = _mm_set1_ps(1.0f / (0x7fff * 0x10000));
   size_t i;
   for (i = 0; i + 8 <= samples; i += 8, in += 8, out += 8)
   {
      __m128i input = _mm_loadu_si128((const __m128i *)in);
      __m128i regs[2] = {
         _mm_unpacklo_epi16(_mm_setzero_si128(), input),
         _mm_unpackhi_epi16(_mm_setzero_si128(), input),
      };

      __m128 output[2] = {
         _mm_mul_ps(_mm_cvtepi32_ps(regs[0]), factor),
         _mm_mul_ps(_mm_cvtepi32_ps(regs[1]), factor),
      };

      _mm_storeu_ps(out + 0, output[0]);
      _mm_storeu_ps(out + 4, output[1]);
   }

   audio_convert_s16_to_float_C(out, in, samples - i);
}

void audio_convert_float_to_s16_SSE2(int16_t *out,
      const float *in, size_t samples)
{
   __m128 factor = _mm_set1_ps((float)0x7fff);
   size_t i;
   for (i = 0; i + 8 <= samples; i += 8, in += 8, out += 8)
   {
      __m128 input[2] = { _mm_loadu_ps(in + 0), _mm_loadu_ps(in + 4) };
      __m128 res[2] = { _mm_mul_ps(input[0], factor), _mm_mul_ps(input[1], factor) };

      __m128i ints[2] = { _mm_cvtps_epi32(res[0]), _mm_cvtps_epi32(res[1]) };
      __m128i packed = _mm_packs_epi32(ints[0], ints[1]);

      _mm_storeu_si128((__m128i *)out, packed);
   }

   audio_convert_float_to_s16_C(out, in, samples - i);
}
#elif __ALTIVEC__
void audio_convert_s16_to_float_altivec(float *out,
      const int16_t *in, size_t samples)
{
   // Unaligned loads/store is a bit expensive, so we optimize for the good path (very likely).
   if (((uintptr_t)out & 15) + ((uintptr_t)in & 15) == 0)
   {
      size_t i;
      for (i = 0; i + 8 <= samples; i += 8, in += 8, out += 8)
      {
         vector signed short input = vec_ld(0, in);
         vector signed int hi = vec_unpackh(input);
         vector signed int lo = vec_unpackl(input);
         vector float out_hi = vec_ctf(hi, 15);
         vector float out_lo = vec_ctf(lo, 15);

         vec_st(out_hi,  0, out);
         vec_st(out_lo, 16, out);
      }

      audio_convert_s16_to_float_C(out, in, samples - i);
   }
   else
      audio_convert_s16_to_float_C(out, in, samples);
}

void audio_convert_float_to_s16_altivec(int16_t *out,
      const float *in, size_t samples)
{
   // Unaligned loads/store is a bit expensive, so we optimize for the good path (very likely).
   if (((uintptr_t)out & 15) + ((uintptr_t)in & 15) == 0)
   {
      size_t i;
      for (i = 0; i + 8 <= samples; i += 8, in += 8, out += 8)
      {
         vector float input0 = vec_ld( 0, in);
         vector float input1 = vec_ld(16, in);
         vector signed int result0 = vec_cts(input0, 15);
         vector signed int result1 = vec_cts(input1, 15);
         vec_st(vec_packs(result0, result1), 0, out);
      }

      audio_convert_float_to_s16_C(out, in, samples - i);
   }
   else
      audio_convert_float_to_s16_C(out, in, samples);
}

#endif

