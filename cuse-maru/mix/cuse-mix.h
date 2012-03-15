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

#ifndef CUSE_MIX_H__
#define CUSE_MIX_H__

#include <libmaru/fifo.h>
#include "resampler.h"
#include <stdbool.h>
#include <stdint.h>

struct stream_info
{
   bool active;
   maru_fifo *fifo;
   char process_name[256];

   struct fuse_pollhandle *ph;
   int sync_fd;

   int sample_rate;
   int channels;
   int bits;

   int frags;
   int fragsize;
   uint64_t write_cnt;

   int volume;
   float volume_f;

   bool nonblock;

   maru_resampler_t *src;
};

struct global
{
   int dev;
   int epfd;
   int ping_fd;
   pthread_mutex_t lock;

   struct
   {
      int fragsize;
      int sample_rate;
      int channels;
      int bits;
      int format;

      unsigned hw_frags;
      unsigned hw_fragsize;
      unsigned sw_frags;
      unsigned sw_fragsize;
   } format;

#define MAX_STREAMS 16
   struct stream_info stream_info[MAX_STREAMS];
};

extern struct global g_state;

void global_lock(void);
void global_unlock(void);

void stream_poll_signal(struct stream_info *info);

#endif

