#include "../../fifo.h"
#include "cuse-mix.h"
#include "mixthread.h"
#include "control.h"

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
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <limits.h>

struct global g_state;

void global_lock(void)
{
   pthread_mutex_lock(&g_state.lock);
}

void global_unlock(void)
{
   pthread_mutex_unlock(&g_state.lock);
}

#define HW_FRAGS 4
#define HW_FRAGSHIFT 10

static bool set_hw_formats(void)
{
   int frag = (HW_FRAGS << 16) | HW_FRAGSHIFT;
   if (ioctl(g_state.dev, SNDCTL_DSP_SETFRAGMENT, &frag) < 0)
   {
      perror("ioctl");
      return false;
   }

   if (ioctl(g_state.dev, SNDCTL_DSP_GETBLKSIZE, &g_state.format.fragsize) < 0)
   {
      perror("ioctl");
      return false;
   }

   g_state.format.sample_rate = 48000;
   if (ioctl(g_state.dev, SNDCTL_DSP_SPEED, &g_state.format.sample_rate) < 0)
   {
      perror("ioctl");
      return false;
   }

   g_state.format.channels = 2;
   if (ioctl(g_state.dev, SNDCTL_DSP_CHANNELS, &g_state.format.channels) < 0)
   {
      perror("ioctl");
      return false;
   }

   g_state.format.bits = 16;
   g_state.format.format = AFMT_S16_LE;
   if (ioctl(g_state.dev, SNDCTL_DSP_SETFMT, &g_state.format.format) < 0)
   {
      perror("ioctl");
      return false;
   }

   return true;
}

static void get_process_name(char *name, size_t size, pid_t pid)
{
   if (size == 0)
      return;

   char proc_path[PATH_MAX];
   snprintf(proc_path, sizeof(proc_path), "/proc/%lu/cmdline", (unsigned long)pid);
   int fd = open(proc_path, O_RDONLY);
   if (fd < 0)
   {
      snprintf(name, size, "Unknown");
      return;
   }

   ssize_t ret = read(fd, name, size - 1);
   if (ret < 0)
      ret = 0;

   name[ret] = '\0';
   close(fd);
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
      if (!g_state.stream_info[i].active)
      {
         g_state.stream_info[i].active = true;
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

   struct stream_info *stream_info = &g_state.stream_info[info->fh];

   stream_info->sample_rate = g_state.format.sample_rate;
   stream_info->channels = g_state.format.channels;
   stream_info->bits = g_state.format.bits;

   stream_info->fragsize = 4096;
   stream_info->frags = 4;

   info->nonseekable = 1;
   info->direct_io = 1;

   const struct fuse_ctx *ctx = fuse_req_ctx(req);
   if (ctx)
   {
      get_process_name(stream_info->process_name,
            sizeof(stream_info->process_name), ctx->pid);
   }

   fuse_reply_open(req, info);
}

static bool init_stream(struct stream_info *stream_info)
{
   maru_fifo *fifo = maru_fifo_new(stream_info->frags * stream_info->fragsize);
   if (!fifo)
      return false;

   maru_fifo_set_read_trigger(fifo, g_state.format.fragsize);

   if (stream_info->sample_rate != g_state.format.sample_rate)
   {
      resampler_init(&stream_info->src, fifo,
            stream_info->sample_rate, g_state.format.sample_rate);

      stream_info->src_active = true;
   }
   else
      stream_info->src_active = false;

   int ret = epoll_ctl(g_state.epfd, EPOLL_CTL_ADD, maru_fifo_read_notify_fd(fifo),
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
   global_unlock();
   return true;
}

static void reset_stream(struct stream_info *stream_info)
{
   maru_fifo *fifo = stream_info->fifo;
   if (!fifo)
      return;

   epoll_ctl(g_state.epfd, EPOLL_CTL_DEL, maru_fifo_read_notify_fd(fifo), NULL);

   global_lock();
   stream_info->fifo = NULL;
   global_unlock();

   maru_fifo_free(fifo);

   eventfd_write(g_state.ping_fd, 1);
}

static void maru_write(fuse_req_t req, const char *data, size_t size,
      off_t off, struct fuse_file_info *info)
{
   struct stream_info *stream_info = &g_state.stream_info[info->fh];

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
      fuse_reply_write(req, ret);
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
   struct stream_info *stream_info = &g_state.stream_info[info->fh];

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
   struct stream_info *stream_info = &g_state.stream_info[info->fh];

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

         // Don't really support downsampling, so cap the sample rate.
         if (i > g_state.format.sample_rate)
            i = g_state.format.sample_rate;

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
               i = AFMT_S16_LE; // Don't support format conversion yet, so take the happy path.
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

         if (fragsize < g_state.format.fragsize)
         {
            fprintf(stderr, "*** Fragsize set is too low: %d bytes!\n", fragsize);
            fragsize = g_state.format.fragsize;
         }

         if (frags < 2)
         {
            fprintf(stderr, "*** Number of fragments is too low: %d frags!\n", frags);
            frags = 2;
         }

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

         if (ioctl(g_state.dev, SNDCTL_DSP_GETODELAY, &i) < 0)
         {
            fuse_reply_err(req, EINVAL);
            break;
         }

         // Compensate for possibly different sample rates.
         i = i * stream_info->sample_rate / g_state.format.sample_rate;

         if (stream_info->fifo)
            i += maru_fifo_buffered_size(stream_info->fifo);

         IOCTL_RETURN(&i);
         break;
      }
#endif

#ifdef SNDCTL_DSP_SYNC
      case SNDCTL_DSP_SYNC:
      {
         fprintf(stderr, "SYNC\n");
         if (ioctl(g_state.dev, SNDCTL_DSP_GETODELAY, &i) < 0)
         {
            fuse_reply_err(req, EINVAL);
            break;
         }

         i = i * stream_info->sample_rate / g_state.format.sample_rate;

         if (stream_info->fifo)
            i += maru_fifo_buffered_size(stream_info->fifo);

         uint64_t usec = (i * UINT64_C(1000000)) / (stream_info->sample_rate * stream_info->channels * stream_info->bits / 8);
         usleep(usec);

         IOCTL_RETURN_NULL();
         fprintf(stderr, "SYNC END\n");
         break;
      }
#endif

#ifdef SNDCTL_DSP_GETOPTR
      case SNDCTL_DSP_GETOPTR:
      {
         global_lock();
         count_info ci = {
            .bytes  = stream_info->write_cnt,
            .blocks = stream_info->write_cnt / stream_info->fragsize,
            .ptr    = stream_info->write_cnt % (stream_info->fragsize * stream_info->frags),
         };
         global_unlock();

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
         i &= 0xff;

         if (i > 100)
            i = 100;
         else if (i < 0)
            i = 0;

         global_lock();
         stream_info->volume = i;
         stream_info->volume_f = i / 100.0f;
         global_unlock();

         i |= i << 8;

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
         i |= i << 8;
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
   struct stream_info *stream_info = &g_state.stream_info[info->fh];

   reset_stream(stream_info);

   if (stream_info->ph)
      fuse_pollhandle_destroy(stream_info->ph);

   // Keep volume for stream.
   int vol = stream_info->volume;
   float vol_f = stream_info->volume_f;

   memset(stream_info, 0, sizeof(*stream_info));

   stream_info->volume = vol;
   stream_info->volume_f = vol_f;

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
   FUSE_OPT_KEY("-D", 1),
   FUSE_OPT_KEY("--daemon", 1),
   FUSE_OPT_END
};

static void print_help(void)
{
   fprintf(stderr, "CUSE-ROSS Usage:\n");
   fprintf(stderr, "\t-M major, --maj=major\n");
   fprintf(stderr, "\t-m minor, --min=minor\n");
   fprintf(stderr, "\t-n name, --name=name (default: marumix)\n");
   fprintf(stderr, "\t--sink=device (default: /dev/maru)\n");
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

static bool init_cuse_mix(const char *sink_name)
{
   g_state.dev = open(sink_name, O_WRONLY);
   if (g_state.dev < 0)
   {
      perror("open");
      return false;
   }

   if (!set_hw_formats())
   {
      fprintf(stderr, "Cannot set HW formats ...\n");
      return false;
   }

   g_state.epfd = epoll_create(MAX_STREAMS);
   if (g_state.epfd < 0)
   {
      perror("epoll_create");
      return false;
   }

   g_state.ping_fd = eventfd(0, 0);
   if (g_state.ping_fd < 0)
   {
      perror("eventfd");
      return false;
   }

   epoll_ctl(g_state.epfd, EPOLL_CTL_ADD, g_state.ping_fd,
         &(struct epoll_event) {
            .events = POLLIN,
         });

   if (pthread_mutex_init(&g_state.lock, NULL) < 0)
   {
      perror("pthread_mutex_init");
      return false;
   }

   for (unsigned i = 0; i < MAX_STREAMS; i++)
   {
      g_state.stream_info[i].volume = 100;
      g_state.stream_info[i].volume_f = 1.0f;
   }

   if (!start_mix_thread())
      return false;

   if (!start_control_thread())
      return false;

   return true;
}

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
   fuse_opt_add_arg(&args, "-f");

   snprintf(dev_name, sizeof(dev_name), "DEVNAME=%s",
         param.dev_name ? param.dev_name : "marumix");

   struct cuse_info ci = {
      .dev_major = param.major,
      .dev_minor = param.minor,
      .dev_info_argc = 1,
      .dev_info_argv = dev_info_argv,
      .flags = CUSE_UNRESTRICTED_IOCTL,
   };

   if (!init_cuse_mix(param.sink_name ? param.sink_name : "/dev/maru"))
      return 1;

   return cuse_lowlevel_main(args.argc, args.argv, &ci, &maru_op, NULL);
}

