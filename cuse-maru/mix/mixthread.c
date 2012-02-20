#include "cuse-mix.h"
#include "mixthread.h"
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <sys/soundcard.h>
#include <sys/ioctl.h>

static pthread_t g_thread;

long src_callback(void *cb_data, float **data)
{
   struct stream_info *info = cb_data;

   memset(info->src_data_i, 0, info->fragsize);

   ssize_t has_read = maru_fifo_read(info->fifo, info->src_data_i, info->fragsize);
   if (has_read > 0)
      info->write_cnt += has_read;

   src_short_to_float_array(info->src_data_i, info->src_data_f,
         info->fragsize / sizeof(int16_t));

   *data = info->src_data_f;
   return info->fragsize / (info->channels * info->bits / 8);
}

static bool write_all(int fd, const void *data_, size_t size)
{
   const uint8_t *data = data_;

   while (size)
   {
      ssize_t ret = write(fd, data, size);
      if (ret <= 0)
         return false;

      data += ret;
      size -= ret;
   }

   return true;
}

static void mix_streams(const struct epoll_event *events, size_t num_events,
      int16_t *mix_buffer,
      size_t fragsize)
{
   size_t samples = fragsize / (g_state.format.bits / 8);

   float tmp_mix_buffer_f[samples];
   int16_t tmp_mix_buffer_i[samples];

   float mix_buffer_f[samples];

   memset(mix_buffer_f, 0, sizeof(mix_buffer_f));

   global_lock();
   for (unsigned i = 0; i < num_events; i++)
   {
      memset(tmp_mix_buffer_f, 0, sizeof(tmp_mix_buffer_f));

      struct stream_info *info = events[i].data.ptr;

      // We were pinged, clear eventfd.
      if (!info)
      {
         uint64_t dummy;
         eventfd_read(g_state.ping_fd, &dummy);
         continue;
      }

      if (info->fifo)
      {
         if (info->src)
         {
            src_callback_read(info->src,
                  (double)g_state.format.sample_rate / info->sample_rate,
                  samples / info->channels,
                  tmp_mix_buffer_f);
         }
         else
         {
            ssize_t has_read = maru_fifo_read(info->fifo, tmp_mix_buffer_i, fragsize);

            if (has_read > 0)
               info->write_cnt += has_read;

            src_short_to_float_array(tmp_mix_buffer_i, tmp_mix_buffer_f, samples);
         }

         maru_fifo_read_notify_ack(info->fifo);
      }

      for (size_t i = 0; i < samples; i++)
         mix_buffer_f[i] += tmp_mix_buffer_f[i] * info->volume_f;
   }
   global_unlock();

   src_float_to_short_array(mix_buffer_f, mix_buffer, samples);
}

static void *thread_entry(void *data)
{
   (void)data;

   int16_t *mix_buffer = malloc(g_state.format.fragsize);
   if (!mix_buffer)
   {
      fprintf(stderr, "Failed to allocate mixbuffer.\n");
      exit(1);
   }

   for (;;)
   {
      struct epoll_event events[MAX_STREAMS];

      int ret = epoll_wait(g_state.epfd, events, MAX_STREAMS, -1);
      if (ret < 0)
      {
         if (errno == EINTR)
            continue;

         perror("epoll_wait");
         exit(1);
      }

      mix_streams(events, ret, mix_buffer, g_state.format.fragsize);

      if (!write_all(g_state.dev, mix_buffer, g_state.format.fragsize))
      {
         fprintf(stderr, "write_all failed!\n");
         exit(1);
      }

      memset(mix_buffer, 0, g_state.format.fragsize);
   }

   free(mix_buffer);
   return NULL;
}

bool start_mix_thread(void)
{
   if (pthread_create(&g_thread, NULL, thread_entry, NULL) < 0)
   {
      perror("pthread_create");
      return false;
   }

   return true;
}

