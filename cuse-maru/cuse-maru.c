#include <libmaru.h>
#include "utils.h"

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

#define MAX_STREAMS 8

static maru_context *g_ctx;
static maru_volume g_min_volume;
static maru_volume g_max_volume;
static int g_sample_rate;
static int g_frags;
static int g_fragsize;

struct stream_info
{
   bool active;

   maru_stream stream;
   pthread_mutex_t lock;
   struct fuse_pollhandle *ph;

   volatile sig_atomic_t error;

   int sample_rate;
   int channels;
   int bits;

   int fragsize;
   int frags;

   bool nonblock;

   uint32_t write_cnt;
};

static struct stream_info g_stream_info[MAX_STREAMS];

static void maru_open(fuse_req_t req, struct fuse_file_info *info)
{
   if ((info->flags & (O_WRONLY | O_RDONLY | O_RDWR)) != O_WRONLY)
   {
      fuse_reply_err(req, EACCES);
      return;
   }

   bool found = false;
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

   if (!found)
   {
      fuse_reply_err(req, EBUSY);
      return;
   }

   struct stream_info *stream_info = &g_stream_info[info->fh];

   pthread_mutex_init(&stream_info->lock, NULL);

   // Just set some defaults.
   stream_info->sample_rate = g_sample_rate;
   stream_info->channels = 2;
   stream_info->bits = 16;
   stream_info->stream = LIBMARU_STREAM_MASTER; // Invalid stream for writing.

   stream_info->fragsize = g_fragsize;
   stream_info->frags = g_frags;

   info->nonseekable = 1;
   info->direct_io = 1;
   fuse_reply_open(req, info);
}

static void write_notification_cb(void *data)
{
   struct stream_info *info = data;
   pthread_mutex_lock(&info->lock);

   if (info->ph)
   {
      fuse_lowlevel_notify_poll(info->ph);
      fuse_pollhandle_destroy(info->ph);
      info->ph = NULL;
   }

   pthread_mutex_unlock(&info->lock);
}

static bool init_stream(struct stream_info *info)
{
   int stream = maru_find_available_stream(g_ctx);
   if (stream < 0)
      return false;

   maru_error err = maru_stream_open(g_ctx, stream,
         &(const struct maru_stream_desc) {
            .sample_rate   = info->sample_rate,
            .channels      = info->channels,
            .bits          = info->bits,
            .fragment_size = info->fragsize,
            .buffer_size   = info->fragsize * info->frags,
         });

   if (err != LIBMARU_SUCCESS)
      return false;

   maru_stream_set_write_notification(g_ctx, stream, write_notification_cb, info);

   info->stream = stream;
   return true;
}

static void maru_write(fuse_req_t req, const char *data, size_t size, off_t off,
      struct fuse_file_info *info)
{
   struct stream_info *stream_info = &g_stream_info[info->fh];

   if (stream_info->error)
   {
      fuse_reply_err(req, EPIPE);
      return;
   }

   if (size == 0)
   {
      fuse_reply_write(req, 0);
      return;
   }

   // First write, we have to open stream after ioctl() calls are made.
   if (stream_info->stream == LIBMARU_STREAM_MASTER && !init_stream(stream_info))
   {
      fuse_reply_err(req, EBUSY);
      return;
   }

   bool nonblock = info->flags & O_NONBLOCK || stream_info->nonblock;

   size_t to_write;
   if (nonblock)
   {
      to_write = maru_stream_write_avail(g_ctx, stream_info->stream);
      unsigned frame_size = stream_info->channels * stream_info->bits / 8;
      to_write /= frame_size;
      to_write *= frame_size;
      if (to_write > size)
         to_write = size;
   }
   else
      to_write = size;

   if (to_write == 0)
   {
      fuse_reply_err(req, EAGAIN);
      return;
   }

   size_t ret = maru_stream_write(g_ctx, stream_info->stream, data, to_write);

   if (ret == 0)
      fuse_reply_err(req, EIO);
   else
   {
      fuse_reply_write(req, ret);
      stream_info->write_cnt += ret;
   }
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
         i = DSP_CAP_REALTIME | (maru_get_num_streams(g_ctx) > 1 ? DSP_CAP_MULTI : 0);
         IOCTL_RETURN(&i);
         break;
#endif

#ifdef SNDCTL_DSP_RESET
      case SNDCTL_DSP_RESET:
#if defined(SNDCTL_DSP_HALT) && (SNDCTL_DSP_HALT != SNDCTL_DSP_RESET)
      case SNDCTL_DSP_HALT:
#endif
         if (stream_info->stream != LIBMARU_STREAM_MASTER)
         {
            maru_stream_close(g_ctx, stream_info->stream);
            stream_info->stream = LIBMARU_STREAM_MASTER;
            stream_info->write_cnt = 0;
         }
         IOCTL_RETURN_NULL();
         break;
#endif

#ifdef SNDCTL_DSP_SPEED
      case SNDCTL_DSP_SPEED:
         PREP_UARG_INOUT(&i, &i);
         i = stream_info->sample_rate; // Hack for now.
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
               i = AFMT_S16_LE; // USB spec is little endian only.
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
         size_t write_avail = stream_info->fragsize * stream_info->frags - 1;
         if (stream_info->stream != LIBMARU_STREAM_MASTER)
            write_avail = maru_stream_write_avail(g_ctx, stream_info->stream);

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
         if (stream_info->stream != LIBMARU_STREAM_MASTER)
         {
            fuse_reply_err(req, EINVAL);
            break;
         }

         PREP_UARG_INOUT(&i, &i);
         int frags = (i >> 16) & 0xffff;
         int fragsize = 1 << (i & 0xffff);

         if (fragsize < 512 || frags < 2)
         {
            fuse_reply_err(req, EINVAL);
            break;
         }

         stream_info->fragsize = fragsize;
         stream_info->frags    = next_pot(frags);

         IOCTL_RETURN(&i);
         break;
      }
#endif

#ifdef SNDCTL_DSP_GETODELAY
      case SNDCTL_DSP_GETODELAY:
      {
         PREP_UARG_OUT(&i);
         maru_usec lat = maru_stream_current_latency(g_ctx, stream_info->stream);

         if (lat < 0)
            i = 0;
         else
            i = (lat * stream_info->sample_rate * stream_info->channels * stream_info->bits / 8) / 1000000;

         IOCTL_RETURN(&i);
         break;
      }
#endif

         // TODO: Add proper support for this in libmaru?
#ifdef SNDCTL_DSP_SYNC
      case SNDCTL_DSP_SYNC:
         if (stream_info->stream != LIBMARU_STREAM_MASTER)
         {
            maru_usec lat = maru_stream_current_latency(g_ctx, stream_info->stream);
            if (lat >= 0)
               usleep(lat);
         }
         IOCTL_RETURN_NULL();
         break;
#endif

#ifdef SNDCTL_DSP_GETOPTR
      case SNDCTL_DSP_GETOPTR:
      {
         size_t driver_write_cnt = stream_info->write_cnt;
         driver_write_cnt -= stream_info->fragsize * stream_info->frags - 1;
         driver_write_cnt += maru_stream_write_avail(g_ctx, stream_info->stream);

         count_info ci = {
            .bytes  = driver_write_cnt,
            .blocks = driver_write_cnt / stream_info->fragsize,
            .ptr    = driver_write_cnt % (stream_info->fragsize * stream_info->frags),
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
         int left = i & 0xff;

         maru_volume min = g_min_volume;
         maru_volume max = g_max_volume;

         maru_volume vol = LIBMARU_VOLUME_MUTE;
         if (i > 0)
            vol = (max * left + min * (100 - left)) / 100;

         if (vol < min && i > 0)
            vol = min;
         else if (vol > max)
            vol = max;

         if (maru_stream_set_volume(g_ctx, LIBMARU_STREAM_MASTER, vol, 0) != LIBMARU_SUCCESS)
         {
            fuse_reply_err(req, EIO);
            break;
         }

         i = (left << 8) | left;
         IOCTL_RETURN(&i);
         break;
      }

      // We really want this no matter what ...
#ifndef SNDCTL_DSP_GETPLAYVOL
#define SNDCTL_DSP_GETPLAYVOL _SIOR('P', 24, int)
#endif
      case SNDCTL_DSP_GETPLAYVOL:
         PREP_UARG_OUT(&i);

         maru_volume cur;
         maru_volume min = g_min_volume;
         maru_volume max = g_max_volume;

         if (maru_stream_get_volume(g_ctx, LIBMARU_STREAM_MASTER,
                  &cur, NULL, NULL, 50000) != LIBMARU_SUCCESS)
         {
            fuse_reply_err(req, EIO);
            break;
         }

         if (min >= max)
            i = 100;
         else if (cur < min)
            i = 0;
         else if (cur > max)
            i = 100;
         else
            i = (100 * (cur - min)) / (max - min);

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

   pthread_mutex_lock(&stream_info->lock);

   maru_update_pollhandle(stream_info, ph);

   if (stream_info->error)
      fuse_reply_poll(req, POLLHUP);
   else if (stream_info->stream == LIBMARU_STREAM_MASTER || maru_stream_write_avail(g_ctx, stream_info->stream))
      fuse_reply_poll(req, POLLOUT);
   else
      fuse_reply_poll(req, 0);

   pthread_mutex_unlock(&stream_info->lock);
}


static void maru_release(fuse_req_t req, struct fuse_file_info *info)
{
   struct stream_info *stream_info = &g_stream_info[info->fh];

   maru_stream_close(g_ctx, stream_info->stream);

   if (stream_info->ph)
      fuse_pollhandle_destroy(stream_info->ph);

   pthread_mutex_destroy(&stream_info->lock);

   memset(stream_info, 0, sizeof(*stream_info));
   fuse_reply_err(req, 0);
}

#define MARU_OPT(t, p) { t, offsetof(struct maru_param, p), 1 }
struct maru_param
{
   unsigned major;
   unsigned minor;
   char *dev_name;

   unsigned hw_frags;
   unsigned hw_fragsize;
   unsigned hw_rate;
};

static const struct fuse_opt maru_opts[] = {
   MARU_OPT("-M %u", major),
   MARU_OPT("--maj=%u", major),
   MARU_OPT("-m %u", minor),
   MARU_OPT("--min=%u", minor),
   MARU_OPT("-n %s", dev_name),
   MARU_OPT("--name=%s", dev_name),
   MARU_OPT("--hw-frags=%u", hw_frags),
   MARU_OPT("--hw-fragsize=%u", hw_fragsize),
   MARU_OPT("--hw-rate=%u", hw_rate),
   FUSE_OPT_KEY("-h", 0),
   FUSE_OPT_KEY("--help", 0),
   FUSE_OPT_KEY("-D", 1),
   FUSE_OPT_KEY("--daemon", 1),
   FUSE_OPT_END
};

static void print_help(void)
{
   fprintf(stderr, "CUSE-ROSS Usage:\n");
   fprintf(stderr, "\t-M major, --maj=major\n");
   fprintf(stderr, "\t-m minor, --min=minor\n");
   fprintf(stderr, "\t-n name, --name=name (default: maru)\n");
   fprintf(stderr, "\t--hw-frags=frags (default: 4)\n");
   fprintf(stderr, "\t--hw-fragsize=fragsize (default: 4096)\n");
   fprintf(stderr, "\t--hw-rate=rate (default: 48000)\n");
   fprintf(stderr, "\t-D, --daemon, run in background\n");
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
      case 1:
         return fuse_daemonize(0);
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
   struct maru_param param = {
      .hw_frags = 4,
      .hw_fragsize = 4096,
      .hw_rate = 48000,
   };

   char dev_name[128] = {0};
   const char *dev_info_argv[] = { dev_name };

   if (fuse_opt_parse(&args, &param, maru_opts, process_arg))
   {
      fprintf(stderr, "Failed to parse ...\n");
      return 1;
   }
   fuse_opt_add_arg(&args, "-f");

   g_frags = next_pot(param.hw_frags);
   g_fragsize = next_pot(param.hw_fragsize);
   g_sample_rate = param.hw_rate;

   snprintf(dev_name, sizeof(dev_name), "DEVNAME=%s", param.dev_name ? param.dev_name : "maru");

   struct cuse_info ci = {
      .dev_major = param.major,
      .dev_minor = param.minor,
      .dev_info_argc = 1,
      .dev_info_argv = dev_info_argv,
      .flags = CUSE_UNRESTRICTED_IOCTL,
   };

   struct maru_audio_device device;
   struct maru_audio_device *list;
   unsigned list_size;
   maru_error err = maru_list_audio_devices(&list, &list_size);
   if (err != LIBMARU_SUCCESS || list_size == 0)
   {
      MARU_LOG_ERROR(err);
      return 1;
   }

   device = list[0];
   free(list);

   err = maru_create_context_from_vid_pid(&g_ctx, device.vendor_id, device.product_id,
         &(const struct maru_stream_desc) { .bits = 16, .channels = 2 });

   if (err != LIBMARU_SUCCESS)
   {
      MARU_LOG_ERROR(err);
      return 1;
   }

   if (maru_stream_get_volume(g_ctx, LIBMARU_STREAM_MASTER,
            NULL, &g_min_volume, &g_max_volume, 50000) != LIBMARU_SUCCESS)
   {
      return 1;
   }

   int ret = cuse_lowlevel_main(args.argc, args.argv, &ci, &maru_op, NULL);
   maru_destroy_context(g_ctx);
   return ret;
}

