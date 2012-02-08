#include "libmaru.h"
#include "fifo.h"
#include <libusb-1.0/libusb.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>

struct maru_stream_internal
{
   maru_fifo *fifo;
   unsigned stream_ep;
   unsigned feedback_ep;

   uint32_t transfer_speed;
   uint32_t transfer_speed_fraction;

   maru_notification_cb write_cb;
   void *write_userdata;
   maru_notification_cb error_cb;
   void *error_userdata;
};

struct poll_list
{
   struct pollfd *fd;
   size_t capacity;
   size_t size;
};

struct maru_context
{
   libusb_context *ctx;
   libusb_device_handle *handle;
   struct libusb_config_descriptor *conf;

   struct maru_stream_internal *streams;
   unsigned num_streams;
   unsigned control_interface;
   unsigned stream_interface;
   int quit_fd[2];

   struct poll_list fds;

   pthread_mutex_t lock;
   pthread_t thread;
};

struct usb_uas_format_descriptor
{
   uint8_t bLength;
   uint8_t bDescriptorType;
   uint8_t bDescriptorSubtype;
   uint8_t bFormatType;
   uint8_t bNrChannels;
   uint8_t nSubFrameSize;
   uint8_t nBitResolution;
   uint8_t bSamFreqType;
   uint8_t tSamFreq[3]; // Hardcoded for one sample rate.
} __attribute__((packed));

#define USB_CLASS_AUDIO                1
#define USB_SUBCLASS_AUDIO_CONTROL     1
#define USB_SUBCLASS_AUDIO_STREAMING   2

#define USB_ENDPOINT_ISOCHRONOUS       0x01
#define USB_ENDPOINT_ASYNC             0x04

#define USB_CLASS_DESCRIPTOR           0x20
#define USB_INTERFACE_DESCRIPTOR_TYPE  0x04
#define USB_FORMAT_DESCRIPTOR_SUBTYPE  0x02
#define USB_FORMAT_TYPE_I              0x01
#define USB_FREQ_TYPE_DIRECT           0x01

static inline bool interface_is_class(const struct libusb_interface_descriptor *iface, unsigned class)
{
   return iface->bInterfaceClass == class;
}

static inline bool interface_is_subclass(const struct libusb_interface_descriptor *iface, unsigned subclass)
{
   return iface->bInterfaceSubClass == subclass;
}

static int find_interface_class_index(const struct libusb_config_descriptor *conf,
      unsigned class, unsigned subclass)
{
   for (unsigned i = 0; i < conf->bNumInterfaces; i++)
   {
      for (int j = 0; j < conf->interface[i].num_altsetting; j++)
      {
         const struct libusb_interface_descriptor *desc =
            &conf->interface[i].altsetting[j];
         if (interface_is_class(desc, USB_CLASS_AUDIO) &&
               interface_is_subclass(desc, USB_SUBCLASS_AUDIO_STREAMING))
         {
            return i; // Altsettings are ignored for now.
         }
      }
   }

   return -1;
}


static bool conf_is_audio_class(const struct libusb_config_descriptor *conf)
{
   return find_interface_class_index(conf, USB_CLASS_AUDIO,
            USB_SUBCLASS_AUDIO_STREAMING) >= 0;
}

static bool device_is_audio_class(libusb_device *dev)
{
   struct libusb_config_descriptor *desc;
   if (libusb_get_active_config_descriptor(dev, &desc) < 0)
      return false;

   bool ret = conf_is_audio_class(desc);
   libusb_free_config_descriptor(desc);
   return ret;
}

static bool fill_vid_pid(libusb_device *dev,
      struct maru_audio_device *audio_dev)
{
   struct libusb_device_descriptor desc;
   if (libusb_get_device_descriptor(dev, &desc) < 0)
      return false;

   audio_dev->vendor_id = desc.idVendor;
   audio_dev->product_id = desc.idProduct;

   return true;
}

static bool enumerate_audio_devices(libusb_device **list, unsigned devices,
      struct maru_audio_device **audio_list, unsigned *num_devices)
{
   size_t audio_devices = 0;
   size_t audio_devices_size = 0;
   struct maru_audio_device *audio_dev = NULL;

   for (ssize_t i = 0; i < devices; i++)
   {
      if (!device_is_audio_class(list[i]))
         continue;

      if (audio_devices >= audio_devices_size)
      {
         audio_devices_size = 2 * audio_devices_size + 1;

         struct maru_audio_device *new_dev = realloc(audio_dev,
               audio_devices_size * sizeof(*new_dev));

         if (!new_dev)
         {
            free(audio_dev);
            return false;
         }

         audio_dev = new_dev;
      }

      if (!fill_vid_pid(list[i], &audio_dev[audio_devices]))
         continue;

      audio_devices++;
   }

   *audio_list = audio_dev;
   *num_devices = audio_devices;
   return true;
}

maru_error maru_list_audio_devices(struct maru_audio_device **audio_list,
      unsigned *num_devices)
{
   libusb_context *ctx;
   if (libusb_init(&ctx) < 0)
      return LIBMARU_ERROR_GENERIC;

   libusb_device **list = NULL;
   ssize_t devices = libusb_get_device_list(ctx, &list);
   if (devices <= 0)
      goto error;

   if (!enumerate_audio_devices(list, devices, audio_list, num_devices))
      goto error;

   libusb_free_device_list(list, true);
   return LIBMARU_SUCCESS;

error:
   if (list)
      libusb_free_device_list(list, true);

   libusb_exit(ctx);
   return LIBMARU_ERROR_MEMORY;
}

static bool poll_list_add(struct poll_list *fds, int fd, short events)
{
   if (fds->size >= fds->capacity)
   {
      fds->capacity = 2 * fds->capacity + 1;
      fds->fd = realloc(fds->fd, fds->capacity * sizeof(*fds->fd));
      if (!fds->fd)
         return false;
   }

   fds->fd[fds->size++] = (struct pollfd) { .fd = fd, .events = events };
   return true;
}

static bool poll_list_remove(struct poll_list *fds, int fd)
{
   for (size_t i = 0; i < fds->size; i++)
   {
      if (fds->fd[i].fd == fd)
      {
         memmove(&fds->fd[i], &fds->fd[i + 1],
               (fds->size - (i + 1)) * sizeof(struct pollfd));
         fds->size--;
         return true;
      }
   }

   return false;
}

static inline void ctx_lock(maru_context *ctx)
{
   pthread_mutex_lock(&ctx->lock);
}

static inline void ctx_unlock(maru_context *ctx)
{
   pthread_mutex_unlock(&ctx->lock);
}

static void poll_added_cb(int fd, short events, void *userdata)
{
   maru_context *ctx = userdata;
   ctx_lock(ctx);
   poll_list_add(&ctx->fds, fd, events);
   ctx_unlock(ctx);
}

static void poll_removed_cb(int fd, void *userdata)
{
   maru_context *ctx = userdata;
   ctx_lock(ctx);
   poll_list_remove(&ctx->fds, fd);
   ctx_unlock(ctx);
}

static bool poll_list_init(maru_context *ctx)
{
   bool ret = true;
   const struct libusb_pollfd **list = libusb_get_pollfds(ctx->ctx);
   if (!list)
      return false;

   const struct libusb_pollfd **tmp = list;
   while (*tmp)
   {
      const struct libusb_pollfd *fd = *tmp;
      if (!poll_list_add(&ctx->fds, fd->fd, fd->events))
      {
         ret = false;
         goto end;
      }
      tmp++;
   }

   // POLLHUP is ignored by poll() in events, but this will simplify our code.
   if (!poll_list_add(&ctx->fds, ctx->quit_fd[0], POLLIN | POLLHUP))
   {
      ret = false;
      goto end;
   }

   libusb_set_pollfd_notifiers(ctx->ctx, poll_added_cb, poll_removed_cb,
         ctx);

end:
   free(list);
   return ret;
}

static void poll_list_deinit(maru_context *ctx)
{
   libusb_set_pollfd_notifiers(ctx->ctx, NULL, NULL, NULL);
   free(ctx->fds.fd);
   memset(&ctx->fds, 0, sizeof(ctx->fds));
}

static bool fd_is_libusb(maru_context *ctx, int fd)
{
   bool ret = true;
   ctx_lock(ctx);

   if (fd == ctx->quit_fd[0])
      ret = false;
   else
   {
      for (unsigned i = 0; i < ctx->num_streams; i++)
      {
         if (ctx->streams[i].fifo &&
               fd == maru_fifo_read_notify_fd(ctx->streams[i].fifo))
         {
            ret = false;
            break;
         }
      }
   }

   ctx_unlock(ctx);
   return ret;
}

static struct maru_stream_internal *fd_to_stream(maru_context *ctx, int fd)
{
   struct maru_stream_internal *ret = NULL;
   ctx_lock(ctx);

   for (unsigned i = 0; i < ctx->num_streams; i++)
   {
      if (ctx->streams[i].fifo &&
            fd == maru_fifo_read_notify_fd(ctx->streams[i].fifo))
      {
         ret = &ctx->streams[i];
         break;
      }
   }

   ctx_unlock(ctx);
   return ret;
}

static void handle_stream(maru_context *ctx, struct maru_stream_internal *stream)
{
   maru_notification_cb cb = NULL;
   void *userdata = NULL;

   ctx_lock(ctx);

   // It is possible that an open stream was suddenly closed.
   // If so, we can catch it here and ignore it.
   if (!stream->fifo)
      goto end;

   // Handle in a dummy way.
   size_t avail = maru_fifo_read_avail(stream->fifo);
   fprintf(stderr, "%zu bytes available for reading!\n", avail);

   // "Read" in a dummy way.
   struct maru_fifo_locked_region region;
   maru_fifo_read_lock(stream->fifo, avail,
         &region);
   maru_fifo_read_unlock(stream->fifo, &region);

   if (avail && stream->write_cb)
   {
      cb = stream->write_cb;
      userdata = stream->write_userdata;
   }

   maru_fifo_read_notify_ack(stream->fifo);

end:
   ctx_unlock(ctx);

   // Call callback without lock applied to attempt to avoid weird deadlocks
   // as much as possible.
   if (cb)
      cb(userdata);
}

static void *thread_entry(void *data)
{
   maru_context *ctx = data;

   bool alive = true;
   while (alive)
   {
      // We need to make a copy as the polling list can be changed
      // mid-poll, possibly causing very awkward behavior.
      // We cannot hold the lock while we're polling as it
      // could easily cause a deadlock.
      ctx_lock(ctx);
      size_t list_size = ctx->fds.size;
      struct pollfd fds[list_size];
      memcpy(fds, ctx->fds.fd, sizeof(fds));
      ctx_unlock(ctx);

      if (poll(fds, list_size, -1) < 0)
      {
         fprintf(stderr, "poll() failed, hide yo kids, hide yo wife!\n");
         break;
      }

      bool libusb_event = false;

      for (size_t i = 0; i < list_size; i++)
      {
         if (!(fds[i].events & fds[i].revents))
            continue;

         struct maru_stream_internal *stream = NULL;

         if (fd_is_libusb(ctx, fds[i].fd))
            libusb_event = true;
         else if ((stream = fd_to_stream(ctx, fds[i].fd)))
            handle_stream(ctx, stream);

         // If read pipe is closed, we should exit ASAP.
         else if (fds[i].fd == ctx->quit_fd[0])
         {
            if (fds[i].revents & (POLLHUP | POLLNVAL | POLLERR))
               alive = false;
         }
      }

      if (libusb_event && libusb_handle_events_timeout(ctx->ctx, &(struct timeval) {0}) < 0)
      {
         fprintf(stderr, "libusb_handle_events_timeout() failed!\n");
         break;
      }
   }

   return NULL;
}

static bool add_stream(maru_context *ctx, unsigned stream_ep, unsigned feedback_ep)
{
   ctx->streams = realloc(ctx->streams, (ctx->num_streams + 1) * sizeof(*ctx->streams));
   if (!ctx->streams)
      return false;

   ctx->streams[ctx->num_streams] = (struct maru_stream_internal) {
      .stream_ep = stream_ep,
      .feedback_ep = feedback_ep,
   };

   ctx->num_streams++;
   return true;
}

static void deinit_stream(maru_context *ctx, maru_stream stream)
{
   struct maru_stream_internal *str = &ctx->streams[stream];

   ctx_lock(ctx);

   if (str->fifo)
   {
      poll_list_remove(&ctx->fds,
            maru_fifo_read_notify_fd(str->fifo));

      maru_fifo_free(str->fifo);
      str->fifo = NULL;
   }

   ctx_unlock(ctx);
}

static bool enumerate_endpoints(maru_context *ctx, const struct libusb_config_descriptor *cdesc)
{
   const struct libusb_interface_descriptor *desc =
      &cdesc->interface[ctx->stream_interface].altsetting[0]; // Hardcoded for now.

   // Just assume everything uses async feedback.
   for (unsigned i = 0; i < desc->bNumEndpoints; i++)
   {
      const struct libusb_endpoint_descriptor *endp = &desc->endpoint[i];

      if (endp->bmAttributes == (USB_ENDPOINT_ISOCHRONOUS | USB_ENDPOINT_ASYNC) &&
            !add_stream(ctx, endp->bEndpointAddress, endp->bSynchAddress))
         return false;
   }

   return true;
}

static bool enumerate_streams(maru_context *ctx)
{
   struct libusb_config_descriptor *conf = ctx->conf;

   int ctrl_index = find_interface_class_index(conf,
         USB_CLASS_AUDIO, USB_SUBCLASS_AUDIO_CONTROL);
   int stream_index = find_interface_class_index(conf,
         USB_CLASS_AUDIO, USB_SUBCLASS_AUDIO_STREAMING);

   if (ctrl_index < 0 || stream_index < 0)
      return false;

   ctx->control_interface = ctrl_index;
   ctx->stream_interface = stream_index;

   int ifaces[2] = { ctrl_index, stream_index };
   for (unsigned i = 0; i < 2; i++)
   {
      if (libusb_kernel_driver_active(ctx->handle,
               ifaces[i]))
      {
         if (libusb_detach_kernel_driver(ctx->handle,
                  ifaces[i]) < 0)
            return false;
      }

      if (libusb_claim_interface(ctx->handle, ifaces[i]) < 0)
         return false;
   }

   if (!enumerate_endpoints(ctx, conf))
      return false;

   return true;
}

maru_error maru_create_context_from_vid_pid(maru_context **ctx,
      uint16_t vid, uint16_t pid)
{
   maru_context *context = calloc(1, sizeof(*context));
   if (!context)
      return LIBMARU_ERROR_MEMORY;

   context->quit_fd[0] = context->quit_fd[1] = -1;
   if (pipe(context->quit_fd) < 0)
      goto error;

   if (libusb_init(&context->ctx) < 0)
      goto error;

   context->handle = libusb_open_device_with_vid_pid(context->ctx, vid, pid);
   if (!context->handle)
      goto error;

   if (libusb_get_active_config_descriptor(libusb_get_device(context->handle), &context->conf) < 0)
   {
      context->conf = NULL;
      goto error;
   }

   if (!conf_is_audio_class(context->conf))
      goto error;

   if (!enumerate_streams(context))
      goto error;

   if (!poll_list_init(context))
      goto error;

   if (pthread_mutex_init(&context->lock, NULL) < 0)
      goto error;

   if (pthread_create(&context->thread, NULL, thread_entry, context) < 0)
   {
      context->thread = 0;
      goto error;
   }

   *ctx = context;
   return LIBMARU_SUCCESS;

error:
   maru_destroy_context(context);
   return LIBMARU_ERROR_GENERIC;
}

void maru_destroy_context(maru_context *ctx)
{
   if (!ctx)
      return;

   if (ctx->quit_fd[1] >= 0)
   {
      close(ctx->quit_fd[1]);
      if (ctx->thread)
         pthread_join(ctx->thread, NULL);
   }
   if (ctx->quit_fd[0] >= 0)
      close(ctx->quit_fd[0]);

   poll_list_deinit(ctx);

   for (unsigned i = 0; i < ctx->num_streams; i++)
      deinit_stream(ctx, i);
   free(ctx->streams);

   pthread_mutex_destroy(&ctx->lock);

   if (ctx->conf)
      libusb_free_config_descriptor(ctx->conf);

   free(ctx);
}

int maru_get_num_streams(maru_context *ctx)
{
   return ctx->num_streams;
}

static bool parse_audio_format(const uint8_t *data, size_t length,
      struct maru_stream_desc *desc)
{
   const struct usb_uas_format_descriptor *header = NULL;
   for (size_t i = 0; i < length; i += header->bLength)
   {
      header = (const struct usb_uas_format_descriptor*)&data[i];

      if (header->bLength != sizeof(*header))
         continue;

      if (header->bDescriptorType != (USB_CLASS_DESCRIPTOR | USB_INTERFACE_DESCRIPTOR_TYPE) ||
            header->bDescriptorSubtype != USB_FORMAT_DESCRIPTOR_SUBTYPE ||
            header->bFormatType != USB_FORMAT_TYPE_I ||
            header->bSamFreqType != USB_FREQ_TYPE_DIRECT)
         continue;

      desc->sample_rate =
         (header->tSamFreq[0] <<  0) |
         (header->tSamFreq[1] <<  8) |
         (header->tSamFreq[2] << 16);

      desc->channels = header->bNrChannels;
      desc->bits = header->nBitResolution;
      desc->sample_rate_min = desc->sample_rate_max = 0;
      return true;
   }

   return false;
}

static bool fill_audio_format(maru_context *ctx, struct maru_stream_desc *desc)
{
   // Format descriptors are not standard (class specific),
   // so we poke in the extra descriptors.

   const struct libusb_interface_descriptor *iface = &ctx->conf->interface[ctx->stream_interface].altsetting[0];
   return parse_audio_format(iface->extra, iface->extra_length, desc);
}

maru_error maru_get_stream_desc(maru_context *ctx,
      maru_stream stream, struct maru_stream_desc **desc,
      unsigned *num_desc)
{
   if (stream >= maru_get_num_streams(ctx))
      return LIBMARU_ERROR_INVALID;

   struct maru_stream_desc *audio_desc = calloc(1, sizeof(*audio_desc));
   if (!audio_desc)
      goto error;

   // Assume for now that all streams have same audio format.
   if (!fill_audio_format(ctx, audio_desc))
      goto error;

   *desc = audio_desc;
   *num_desc = 1;
   return LIBMARU_SUCCESS;

error:
   free(audio_desc);
   *desc = NULL;
   *num_desc = 0;
   return LIBMARU_ERROR_GENERIC;
}

int maru_is_stream_available(maru_context *ctx, maru_stream stream)
{
   ctx_lock(ctx);

   int ret = LIBMARU_ERROR_INVALID;
   if (stream < ctx->num_streams)
      ret = ctx->streams[stream].fifo ? 1 : 0;

   ctx_unlock(ctx);
   return ret;
}

int maru_find_available_stream(maru_context *ctx)
{
   for (unsigned i = 0; i < ctx->num_streams; i++)
      if (maru_is_stream_available(ctx, i))
         return i;

   return LIBMARU_ERROR_BUSY;
}

void maru_stream_set_write_notification(maru_context *ctx,
      maru_stream stream,
      maru_notification_cb callback, void *userdata)
{
   if (stream >= ctx->num_streams)
      return;

   ctx_lock(ctx);
   ctx->streams[stream].write_cb = callback;
   ctx->streams[stream].write_userdata = userdata;
   ctx_unlock(ctx);
}

void maru_stream_set_error_notification(maru_context *ctx,
      maru_stream stream,
      maru_notification_cb callback, void *userdata)
{
   if (stream >= ctx->num_streams)
      return;

   ctx_lock(ctx);
   ctx->streams[stream].error_cb = callback;
   ctx->streams[stream].error_userdata = userdata;
   ctx_unlock(ctx);
}

