#ifndef AUDIO_UTILS_H
#define AUDIO_UTILS_H

#include <stdint.h>
#include <stddef.h>

#define AUDIO_ALIGNED __attribute__((aligned(16)))

#if __SSE2__
#define audio_convert_s16_to_float audio_convert_s16_to_float_SSE2
#define audio_convert_float_to_s16 audio_convert_float_to_s16_SSE2
#define audio_mix_volume           audio_mix_volume_SSE2

void audio_convert_s16_to_float_SSE2(float *out,
      const int16_t *in, size_t samples);

void audio_convert_float_to_s16_SSE2(int16_t *out,
      const float *in, size_t samples);

void audio_mix_volume_SSE2(float *out,
      const float *in, float vol, size_t samples);

#elif __ALTIVEC__
#define audio_convert_s16_to_float audio_convert_s16_to_float_altivec
#define audio_convert_float_to_s16 audio_convert_float_to_s16_altivec

void audio_convert_s16_to_float_altivec(float *out,
      const int16_t *in, size_t samples);

void audio_convert_float_to_s16_altivec(int16_t *out,
      const float *in, size_t samples);

#else
#define audio_convert_s16_to_float audio_convert_s16_to_float_C
#define audio_convert_float_to_s16 audio_convert_float_to_s16_C
#define audio_mix_volume           audio_mix_volume_C
#endif

void audio_convert_s16_to_float_C(float *out,
      const int16_t *in, size_t samples);
void audio_convert_float_to_s16_C(int16_t *out,
      const float *in, size_t samples);

void audio_mix_volume_C(float *dst, const float *src, float vol, size_t samples);

#endif

