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

