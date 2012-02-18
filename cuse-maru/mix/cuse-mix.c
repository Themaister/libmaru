#include "../../fifo.h"

#include <sys/soundcard.h>
#include <cuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/poll.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <samplerate.h>

#define MAX_STREAMS 16

struct stream_info
{
   bool active;
   maru_fifo *fifo;

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

static int g_dev;
static int g_epfd;
static pthread_mutex_t g_lock;
static pthread_t g_thread;
static struct stream_info g_stream_info[MAX_STREAMS];

static int g_fragsize;
static int g_sample_rate;
static int g_channels;
static int g_bits;
static int g_format;

static inline void global_lock(void)
{
   pthread_mutex_lock(&g_lock);
}

static inline void global_unlock(void)
{
   pthread_mutex_unlock(&g_lock);
}

#define HW_FRAGS 4
#define HW_FRAGSHIFT 11

static bool set_hw_formats(void)
{
   int frag = (HW_FRAGS << 16) | HW_FRAGSHIFT;
   if (ioctl(g_dev, SNDCTL_DSP_SETFRAGMENT, &frag) < 0)
   {
      perror("ioctl");
      return false;
   }

   if (ioctl(g_dev, SNDCTL_DSP_GETBLKSIZE, &g_fragsize) < 0)
   {
      perror("ioctl");
      return false;
   }

   g_sample_rate = 48000;
   if (ioctl(g_dev, SNDCTL_DSP_SPEED, &g_sample_rate) < 0)
   {
      perror("ioctl");
      return false;
   }

   g_channels = 2;
   if (ioctl(g_dev, SNDCTL_DSP_CHANNELS, &g_channels) < 0)
   {
      perror("ioctl");
      return false;
   }

   g_bits = 16;
   g_format = AFMT_S16_LE;
   if (ioctl(g_dev, SNDCTL_DSP_SETFMT, &g_format) < 0)
   {
      perror("ioctl");
      return false;
   }

   return true;
}

static void mix_streams(const struct epoll_event *events, size_t num_events,
      int16_t *mix_buffer,
      size_t fragsize)
{
   size_t samples = fragsize / (g_bits / 8);

   float tmp_mix_buffer_f[samples];
   int16_t tmp_mix_buffer_i[samples];

   float mix_buffer_f[samples];

   memset(mix_buffer_f, 0, sizeof(mix_buffer_f));

   global_lock();
   for (unsigned i = 0; i < num_events; i++)
   {
      memset(tmp_mix_buffer_f, 0, sizeof(tmp_mix_buffer_f));

      struct stream_info *info = events[i].data.ptr;

      if (info->fifo)
      {
         if (info->src)
         {
            src_callback_read(info->src,
                  (double)g_sample_rate / info->sample_rate,
                  samples / info->channels,
                  tmp_mix_buffer_f);
         }
         else
         {
            maru_fifo_read(info->fifo, tmp_mix_buffer_i, fragsize);
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

long src_callback(void *cb_data, float **data)
{
   struct stream_info *info = cb_data;

   memset(info->src_data_i, 0, info->fragsize);
   maru_fifo_read(info->fifo, info->src_data_i, info->fragsize);

   src_short_to_float_array(info->src_data_i, info->src_data_f,
         info->fragsize / sizeof(int16_t));

   *data = info->src_data_f;
   return info->fragsize / (info->channels * info->bits / 8);
}

static void *thread_entry(void *data)
{
   (void)data;

   int16_t *mix_buffer = malloc(g_fragsize);
   if (!mix_buffer)
   {
      fprintf(stderr, "Failed to allocate mixbuffer\n");
      exit(1);
   }

   for (;;)
   {
      struct epoll_event events[MAX_STREAMS];

      int ret;
poll_retry:
      ret = epoll_wait(g_epfd, events, MAX_STREAMS, -1);
      if (ret < 0)
      {
         if (errno == EINTR)
            goto poll_retry;

         perror("epoll_wait");
         exit(1);
      }

      mix_streams(events, ret, mix_buffer, g_fragsize);

      if (!write_all(g_dev, mix_buffer, g_fragsize))
      {
         fprintf(stderr, "write_all failed!\n");
         exit(1);
      }

      memset(mix_buffer, 0, g_fragsize);
   }

   free(mix_buffer);
   return NULL;
}

static void maru_open(fuse_req_t req, struct fuse_file_info *info)
{
   if ((info->flags & (O_WRONLY | O_RDONLY | O_RDWR)) != O_WRONLY)
   {
      fuse_reply_err(req, EACCES);
      return;
   }

   bool found = false;

   global_lock();
   for (unsigned i = 0; i < MAX_STREAMS; i++)
   {
      if (!g_stream_info[i].active)
      {
         g_stream_info[i].active = true;
         info->fh = i;
         found = true;
         break;
      }
   }
   global_unlock();

   if (!found)
   {
      fuse_reply_err(req, EBUSY);
      return;
   }

   struct stream_info *stream_info = &g_stream_info[info->fh];

   stream_info->sample_rate = g_sample_rate;
   stream_info->channels = g_channels;
   stream_info->bits = g_bits;

   stream_info->fragsize = 4096;
   stream_info->frags = 4;

   info->nonseekable = 1;
   info->direct_io = 1;
   fuse_reply_open(req, info);
}

static bool init_stream(struct stream_info *stream_info)
{
   maru_fifo *fifo = maru_fifo_new(stream_info->frags * stream_info->fragsize);
   if (!fifo)
      return false;

   if (stream_info->sample_rate != g_sample_rate)
   {
      stream_info->src_data_f = malloc(stream_info->fragsize * sizeof(float) / sizeof(int16_t));
      stream_info->src_data_i = malloc(stream_info->fragsize);
      assert(stream_info->src_data_f);
      assert(stream_info->src_data_i);

      stream_info->src = src_callback_new(src_callback,
            SRC_SINC_FASTEST, stream_info->channels, NULL, stream_info);

      if (!stream_info->src)
      {
         fprintf(stderr, "Failed to init samplerate ...\n");
         maru_fifo_free(fifo);
         return false;
      }
   }

   int ret = epoll_ctl(g_epfd, EPOLL_CTL_ADD, maru_fifo_read_notify_fd(fifo),
         &(struct epoll_event) {
            .events = POLLIN,
            .data = {
               .ptr = stream_info
            }
         });

   if (ret < 0)
      return false;

   global_lock();
   stream_info->fifo = fifo;
   stream_info->volume = 100;
   stream_info->volume_f = 1.0f;
   global_unlock();
   return true;
}

static void reset_stream(struct stream_info *stream_info)
{
   maru_fifo *fifo = stream_info->fifo;
   if (!fifo)
      return;

   epoll_ctl(g_epfd, EPOLL_CTL_DEL, maru_fifo_read_notify_fd(fifo), NULL);

   global_lock();
   stream_info->fifo = NULL;
   global_unlock();

   if (stream_info->src)
   {
      src_delete(stream_info->src);
      stream_info->src = NULL;

      free(stream_info->src_data_f);
      free(stream_info->src_data_i);
      stream_info->src_data_f = NULL;
      stream_info->src_data_i = NULL;
   }

   maru_fifo_free(fifo);
}

static void maru_write(fuse_req_t req, const char *data, size_t size,
      off_t off, struct fuse_file_info *info)
{
   struct stream_info *stream_info = &g_stream_info[info->fh];

   if (!stream_info->fifo && !init_stream(stream_info))
   {
      fuse_reply_err(req, ENOMEM);
      return;
   }

   ssize_t ret;
   if ((info->flags & O_NONBLOCK) || stream_info->nonblock)
      ret = maru_fifo_write(stream_info->fifo, data, size);
   else
      ret = maru_fifo_blocking_write(stream_info->fifo, data, size);

   if (ret < 0)
      fuse_reply_err(req, EBUSY);
   else
   {
      fuse_reply_write(req, ret);
      stream_info->write_cnt += ret;
   }
}

static void maru_update_pollhandle(struct stream_info *info, struct fuse_pollhandle *ph)
{
   struct fuse_pollhandle *tmp_ph = info->ph;
   info->ph = ph;
   if (tmp_ph)
      fuse_pollhandle_destroy(tmp_ph);
}

static void maru_poll(fuse_req_t req, struct fuse_file_info *info,
      struct fuse_pollhandle *ph)
{
   struct stream_info *stream_info = &g_stream_info[info->fh];

   global_lock();
   maru_update_pollhandle(stream_info, ph);
   global_unlock();

   if (!stream_info->fifo || maru_fifo_write_avail(stream_info->fifo))
      fuse_reply_poll(req, POLLOUT);
   else
      fuse_reply_poll(req, 0);
}

// Almost straight copypasta from OSS Proxy.
// It seems that memory is mapped directly between two different processes.
// Since ioctl() does not contain any size information for its arguments, we first have to tell it how much
// memory we want to map between the two different processes, then ask it to call ioctl() again.
static bool ioctl_prep_uarg(fuse_req_t req,
      void *in, size_t in_size,
      void *out, size_t out_size,
      void *uarg,
      const void *in_buf, size_t in_bufsize, size_t out_bufsize)
{
   bool retry = false;
   struct iovec in_iov = {0};
   struct iovec out_iov = {0};

   if (in)
   {
      if (in_bufsize == 0)
      {
         in_iov.iov_base = uarg;
         in_iov.iov_len = in_size;
         retry = true;
      }
      else
      {
         assert(in_bufsize == in_size);
         memcpy(in, in_buf, in_size);
      }
   }

   if (out)
   {
      if (out_bufsize == 0)
      {
         out_iov.iov_base = uarg;
         out_iov.iov_len = out_size;
         retry = true;
      }
      else
      {
         assert(out_bufsize == out_size);
      }
   }

   if (retry)
      fuse_reply_ioctl_retry(req, &in_iov, 1, &out_iov, 1);

   return retry;
}

#define IOCTL_RETURN(addr) do { \
   fuse_reply_ioctl(req, 0, addr, sizeof(*(addr))); \
} while(0)

#define IOCTL_RETURN_NULL() do { \
   fuse_reply_ioctl(req, 0, NULL, 0); \
} while(0)

#define PREP_UARG(inp, inp_s, outp, outp_s) do { \
   if (ioctl_prep_uarg(req, inp, inp_s, \
            outp, outp_s, uarg, \
            in_buf, in_bufsize, out_bufsize)) \
      return; \
} while(0)

#define PREP_UARG_OUT(outp) PREP_UARG(NULL, 0, outp, sizeof(*(outp)))
#define PREP_UARG_INOUT(inp, outp) PREP_UARG(inp, sizeof(*(inp)), outp, sizeof(*(outp)))


static void maru_ioctl(fuse_req_t req, int signed_cmd, void *uarg,
      struct fuse_file_info *info, unsigned flags,
      const void *in_buf, size_t in_bufsize, size_t out_bufsize)
{
   struct stream_info *stream_info = &g_stream_info[info->fh];

   unsigned cmd = signed_cmd;
   int i = 0;

   switch (cmd)
   {
#ifdef OSS_GETVERSION
      case OSS_GETVERSION:
         PREP_UARG_OUT(&i);
         i = (3 << 16) | (8 << 8) | (1 << 4) | 0; // 3.8.1
         IOCTL_RETURN(&i);
         break;
#endif

#ifdef SNDCTL_DSP_COOKEDMODE
      case SNDCTL_DSP_COOKEDMODE:
         PREP_UARG_INOUT(&i, &i);
         IOCTL_RETURN(&i);
         break;
#endif

#ifdef SNDCTL_DSP_NONBLOCK
      case SNDCTL_DSP_NONBLOCK:
         stream_info->nonblock = true;
         IOCTL_RETURN_NULL();
         break;
#endif

#ifdef SNDCTL_DSP_GETCAPS
      case SNDCTL_DSP_GETCAPS:
         PREP_UARG_OUT(&i);
         i = DSP_CAP_REALTIME | DSP_CAP_MULTI | DSP_CAP_BATCH;
         IOCTL_RETURN(&i);
         break;
#endif

#ifdef SNDCTL_DSP_RESET
      case SNDCTL_DSP_RESET:
#if defined(SNDCTL_DSP_HALT) && (SNDCTL_DSP_HALT != SNDCTL_DSP_RESET)
      case SNDCTL_DSP_HALT:
#endif
         reset_stream(stream_info);
         stream_info->write_cnt = 0;
         IOCTL_RETURN_NULL();
         break;
#endif

#ifdef SNDCTL_DSP_SPEED
      case SNDCTL_DSP_SPEED:
         PREP_UARG_INOUT(&i, &i);

         if (stream_info->fifo)
         {
            fuse_reply_err(req, EINVAL);
            break;
         }

         stream_info->sample_rate = i;
         IOCTL_RETURN(&i);
         break;
#endif

#ifdef SNDCTL_GETFMTS
      case SNDCTL_DSP_GETFMTS: // They essentially do the same thing ...
#endif
#ifdef SNDCTL_DSP_SETFMT
      case SNDCTL_DSP_SETFMT:
         PREP_UARG_INOUT(&i, &i);
         switch (stream_info->bits)
         {
            case 8:
               i = AFMT_U8;
               break;

            case 16:
               i = AFMT_S16_LE;
               break;
         }
         IOCTL_RETURN(&i);
         break;
#endif

#ifdef SNDCTL_DSP_CHANNELS
      case SNDCTL_DSP_CHANNELS:
         PREP_UARG_INOUT(&i, &i);
         i = stream_info->channels;
         IOCTL_RETURN(&i);
         break;
#endif

#ifdef SNDCTL_DSP_STEREO
      case SNDCTL_DSP_STEREO:
      {
         PREP_UARG_INOUT(&i, &i);
         i = stream_info->channels > 1 ? 1 : 0;
         IOCTL_RETURN(&i);
         break;
      }
#endif

#ifdef SNDCTL_DSP_GETOSPACE
      case SNDCTL_DSP_GETOSPACE:
      {
         size_t write_avail = stream_info->fragsize * stream_info->frags;
         if (stream_info->fifo)
            write_avail = maru_fifo_write_avail(stream_info->fifo);

         audio_buf_info audio_info = {
            .bytes      = write_avail,
            .fragments  = write_avail / stream_info->fragsize,
            .fragsize   = stream_info->fragsize,
            .fragstotal = stream_info->frags,
         };

         PREP_UARG_OUT(&audio_info);
         IOCTL_RETURN(&audio_info);
         break;
      }
#endif

#ifdef SNDCTL_DSP_GETBLKSIZE
      case SNDCTL_DSP_GETBLKSIZE:
         PREP_UARG_OUT(&i);
         i = stream_info->fragsize;
         IOCTL_RETURN(&i);
         break;
#endif

#ifdef SNDCTL_DSP_SETFRAGMENT
      case SNDCTL_DSP_SETFRAGMENT:
      {
         if (stream_info->fifo)
         {
            fuse_reply_err(req, EINVAL);
            break;
         }

         PREP_UARG_INOUT(&i, &i);

         int frags = (i >> 16) & 0xffff;
         int fragsize = 1 << (i & 0xffff);

         if (fragsize < 128)
            fragsize = 128;
         if (frags < 4)
            frags = 4;

         stream_info->fragsize = fragsize;
         stream_info->frags    = frags;

         IOCTL_RETURN(&i);
         break;
      }
#endif

#ifdef SNDCTL_DSP_GETODELAY
      case SNDCTL_DSP_GETODELAY:
      {
         PREP_UARG_OUT(&i);

         if (ioctl(g_dev, SNDCTL_DSP_GETODELAY, &i) < 0)
         {
            fuse_reply_err(req, EINVAL);
            break;
         }

         // Compensate for possibly different sample rates.
         i = i * stream_info->sample_rate / g_sample_rate;

         if (stream_info->fifo)
            i += maru_fifo_buffered_size(stream_info->fifo);

         IOCTL_RETURN(&i);
         break;
      }
#endif

#ifdef SNDCTL_DSP_SYNC
      case SNDCTL_DSP_SYNC:
      {
         if (ioctl(g_dev, SNDCTL_DSP_GETODELAY, &i) < 0)
         {
            fuse_reply_err(req, EINVAL);
            break;
         }

         i = i * stream_info->sample_rate / g_sample_rate;

         if (stream_info->fifo)
            i += maru_fifo_buffered_size(stream_info->fifo);

         uint64_t usec = (i * UINT64_C(1000000)) / (stream_info->sample_rate * stream_info->channels * stream_info->bits / 8);
         usleep(usec);

         IOCTL_RETURN_NULL();
         break;
      }
#endif

#ifdef SNDCTL_DSP_GETOPTR
      case SNDCTL_DSP_GETOPTR:
      {
         count_info ci = {
            .bytes  = stream_info->write_cnt,
            .blocks = stream_info->write_cnt / stream_info->fragsize,
            .ptr    = stream_info->write_cnt % (stream_info->fragsize * stream_info->frags),
         };

         PREP_UARG_OUT(&ci);
         IOCTL_RETURN(&ci);
         break;
      }
#endif

      // We really want this no matter what ...
#ifndef SNDCTL_DSP_SETPLAYVOL
#define SNDCTL_DSP_SETPLAYVOL _SIOWR('P', 24, int)
#endif
      case SNDCTL_DSP_SETPLAYVOL:
      {
         PREP_UARG_INOUT(&i, &i);
         if (i > 100)
            i = 100;
         else if (i < 0)
            i = 0;

         global_lock();
         stream_info->volume = i;
         stream_info->volume_f = i / 100.0f;
         global_unlock();

         IOCTL_RETURN(&i);
         break;
      }

      // We really want this no matter what ...
#ifndef SNDCTL_DSP_GETPLAYVOL
#define SNDCTL_DSP_GETPLAYVOL _SIOR('P', 24, int)
#endif
      case SNDCTL_DSP_GETPLAYVOL:
         PREP_UARG_OUT(&i);
         i = stream_info->volume;
         IOCTL_RETURN(&i);
         break;

#ifdef SNDCTL_DSP_SETTRIGGER
      case SNDCTL_DSP_SETTRIGGER:
         // No reason to care about this for now.
         // Maybe when/if mmap() gets implemented.
         PREP_UARG_INOUT(&i, &i);
         IOCTL_RETURN(&i);
         break;
#endif

#ifdef SNDCTL_DSP_POST
      case SNDCTL_DSP_POST:
         IOCTL_RETURN_NULL();
         break;
#endif

      default:
         fuse_reply_err(req, EINVAL);
   }
}

static void maru_release(fuse_req_t req, struct fuse_file_info *info)
{
   struct stream_info *stream_info = &g_stream_info[info->fh];

   reset_stream(stream_info);

   if (stream_info->ph)
      fuse_pollhandle_destroy(stream_info->ph);

   memset(stream_info, 0, sizeof(*stream_info));
   fuse_reply_err(req, 0);
}

#define MARU_OPT(t, p) { t, offsetof(struct maru_param, p), 1 }
struct maru_param
{
   unsigned major;
   unsigned minor;
   char *dev_name;
   char *sink_name;
};

static const struct fuse_opt maru_opts[] = {
   MARU_OPT("-M %u", major),
   MARU_OPT("--maj=%u", major),
   MARU_OPT("-m %u", minor),
   MARU_OPT("--min=%u", minor),
   MARU_OPT("-n %s", dev_name),
   MARU_OPT("--name=%s", dev_name),
   MARU_OPT("--sink=%s", sink_name),
   FUSE_OPT_KEY("-h", 0),
   FUSE_OPT_KEY("--help", 0),
   FUSE_OPT_END
};

static void print_help(void)
{
   fprintf(stderr, "CUSE-ROSS Usage:\n");
   fprintf(stderr, "\t-M major, --maj=major\n");
   fprintf(stderr, "\t-m minor, --min=minor\n");
   fprintf(stderr, "\t-n name, --name=name (default: marumix)\n");
   fprintf(stderr, "\t--sink=device (default: /dev/maru)\n");
   fprintf(stderr, "\t\tDevice will be created in /dev/$name.\n");
   fprintf(stderr, "\n");
}

static int process_arg(void *data, const char *arg, int key,
      struct fuse_args *outargs)
{
   switch (key)
   {
      case 0:
         print_help();
         return fuse_opt_add_arg(outargs, "-ho");
      default:
         return 1;
   }
}

static const struct cuse_lowlevel_ops maru_op = {
   .open    = maru_open,
   .write   = maru_write,
   .ioctl   = maru_ioctl,
   .poll    = maru_poll,
   .release = maru_release,
};

int main(int argc, char *argv[])
{
   struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
   struct maru_param param = {0}; 

   char dev_name[128] = {0};
   const char *dev_info_argv[] = { dev_name };

   if (fuse_opt_parse(&args, &param, maru_opts, process_arg))
   {
      fprintf(stderr, "Failed to parse ...\n");
      return 1;
   }

   snprintf(dev_name, sizeof(dev_name), "DEVNAME=%s",
         param.dev_name ? param.dev_name : "marumix");

   struct cuse_info ci = {
      .dev_major = param.major,
      .dev_minor = param.minor,
      .dev_info_argc = 1,
      .dev_info_argv = dev_info_argv,
      .flags = CUSE_UNRESTRICTED_IOCTL,
   };

   g_dev = open(param.sink_name ? param.sink_name : "/dev/maru", O_WRONLY);
   if (g_dev < 0)
   {
      perror("open");
      return 1;
   }

   if (!set_hw_formats())
   {
      fprintf(stderr, "Cannot set HW formats ...\n");
      return 1;
   }

   g_epfd = epoll_create(MAX_STREAMS);
   if (g_epfd < 0)
   {
      perror("epoll_create");
      return 1;
   }

   if (pthread_mutex_init(&g_lock, NULL) < 0)
   {
      perror("pthread_mutex_init");
      return 1;
   }

   if (pthread_create(&g_thread, NULL, thread_entry, NULL) < 0)
   {
      perror("pthread_create");
      return 1;
   }

   return cuse_lowlevel_main(args.argc, args.argv, &ci, &maru_op, NULL);
}

