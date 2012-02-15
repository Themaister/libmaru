#include <libmaru.h>

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

#define MAX_STREAMS 8

static maru_context *g_ctx;

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

   bool nonblock;
};

static struct stream_info g_stream_info[MAX_STREAMS];

static void maru_open(fuse_req_t req, struct fuse_file_info *info)
{
   if ((info->flags & (O_WRONLY | O_RDONLY | O_RDWR)) != O_WRONLY)
   {
      fuse_reply_err(req, EACCES);
      return;
   }

   int avail_stream = maru_find_available_stream(g_ctx);
   if (avail_stream < 0)
   {
      fuse_reply_err(req, EBUSY);
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
   stream_info->sample_rate = 48000;
   stream_info->channels = 2;
   stream_info->bits = 16;
   stream_info->stream = LIBMARU_STREAM_MASTER; // Invalid stream for writing.

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
            .sample_rate = info->sample_rate,
            .channels    = info->channels,
            .bits        = info->bits,
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

   size_t to_write = size;
   if (nonblock)
      to_write = maru_stream_write_avail(g_ctx, stream_info->stream);

   if (to_write == 0)
   {
      fuse_reply_err(req, EAGAIN);
      return;
   }

   size_t ret = maru_stream_write(g_ctx, stream_info->stream, data, to_write);

   if (ret == 0)
      fuse_reply_err(req, EIO);
   else
      fuse_reply_write(req, ret);
}

static void maru_ioctl(fuse_req_t req, int signed_cmd, void *uarg,
      struct fuse_file_info *info, unsigned flags,
      const void *in_buf, size_t in_bufsize, size_t out_bufsize)
{
   fuse_reply_err(req, ENOSYS);
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
   maru_update_pollhandle(stream_info, NULL);
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
};

static const struct fuse_opt ross_opts[] = {
   MARU_OPT("-M %u", major),
   MARU_OPT("--maj=%u", major),
   MARU_OPT("-m %u", minor),
   MARU_OPT("--min=%u", minor),
   MARU_OPT("-n %s", dev_name),
   MARU_OPT("--name=%s", dev_name),
   FUSE_OPT_KEY("-h", 0),
   FUSE_OPT_KEY("--help", 0),
   FUSE_OPT_END
};

static void print_help(void)
{
   fprintf(stderr, "CUSE-ROSS Usage:\n");
   fprintf(stderr, "\t-M major, --maj=major\n");
   fprintf(stderr, "\t-m minor, --min=minor\n");
   fprintf(stderr, "\t-n name, --name=name (default: ross)\n");
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

   if (fuse_opt_parse(&args, &param, ross_opts, process_arg))
   {
      fprintf(stderr, "Failed to parse ...\n");
      return 1;
   }

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

   int ret = cuse_lowlevel_main(args.argc, args.argv, &ci, &maru_op, NULL);
   maru_destroy_context(g_ctx);
   return ret;
}

