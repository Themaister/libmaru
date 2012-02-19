#ifndef CUSE_MIX_H__
#define CUSE_MIX_H__

#include "../../fifo.h"
#include <stdbool.h>
#include <stdint.h>
#include <samplerate.h>

struct stream_info
{
   bool active;
   maru_fifo *fifo;
   char process_name[256];

   struct fuse_pollhandle *ph;

   int sample_rate;
   int channels;
   int bits;

   int frags;
   int fragsize;
   uint64_t write_cnt;

   int volume;
   float volume_f;

   bool nonblock;

   SRC_STATE *src;
   float *src_data_f;
   int16_t *src_data_i;
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
   } format;

#define MAX_STREAMS 16
   struct stream_info stream_info[MAX_STREAMS];
};

extern struct global g_state;

void global_lock(void);
void global_unlock(void);

#endif

