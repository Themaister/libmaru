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

#ifndef CUSE_MARU_H__
#define CUSE_MARU_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <libmaru/libmaru.h>

#define MAX_STREAMS 8

/** \ingroup internal
 * \brief Structure holding information about a libmaru stream. */
struct cuse_stream_info
{
   /** Set to true if stream is claimed by a file descriptor. */
   bool active;

   /** Name of application using a stream */
   char process_name[PATH_MAX];

   /** libmaru stream. Set to LIBMARU_STREAM_MASTER during configuration stage (before first write()). */
   maru_stream stream;
   /** Lock to let polling notifications occur in a different thread. */
   pthread_mutex_t lock;
   /** Currently held pollhandle. */
   struct fuse_pollhandle *ph;

   /** Set if stream has an error. */
   volatile sig_atomic_t error;

   /** Current sample rate for stream. */
   int sample_rate;
   /** Current number of audio channels for stream. */
   int channels;
   /** Current number of bits in the little-endian PCM format. */
   int bits;

   /** HW fragment size. */
   int fragsize;
   /** HW frags. */
   int frags;

   /** Current known volume of stream (0-100). */
   int vol;

   /** Set if SNDCTL_DSP_NONBLOCK has been explicitly called. */
   bool nonblock;

   /** Number of bytes written to current stream. (Wraps around at 2^32 according to OSS API). */
   uint32_t write_cnt;
};

struct cuse_maru_state
{
   maru_context *ctx;
   pthread_mutex_t lock;
   maru_volume min_volume;
   maru_volume max_volume;
   int sample_rate;
   int frags;
   int fragsize;

   struct cuse_stream_info stream_info[MAX_STREAMS];
};

extern struct cuse_maru_state g_state;

bool read_volume(struct cuse_stream_info *stream);
bool set_volume(struct cuse_stream_info *stream, int vol);

#endif

