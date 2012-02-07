#include "libmaru.h"
#include "fifo.h"
#include <libusb-1.0/libusb.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>

struct maru_stream_internal
{
   maru_fifo *fifo;
   unsigned stream_ep;
   unsigned feedback_ep;

   uint32_t transfer_speed;
   uint32_t transfer_speed_fraction;

   bool busy;
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

   struct maru_stream_internal *streams;
   unsigned num_streams;
   unsigned control_interface;
   unsigned stream_interface;
   int quit_fd[2];

   struct poll_list fds;

   pthread_mutex_t lock;
   pthread_t thread;
};

#define USB_CLASS_AUDIO 1
#define USB_SUBCLASS_AUDIO_CONTROL 1
#define USB_SUBCLASS_AUDIO_STREAMING 2

#define USB_ENDPOINT_ISOCHRONOUS 0x01
#define USB_ENDPOINT_ASYNC 0x04

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

static bool device_is_audio_class(libusb_device *dev)
{
   struct libusb_device_descriptor desc;
   if (libusb_get_device_descriptor(dev, &desc) < 0)
      return false;

   // Audio class is declared in config descriptor.
   if (desc.bDeviceClass != 0 ||
         desc.bDeviceSubClass != 0 ||
         desc.bDeviceProtocol != 0)
      return false;

   struct libusb_config_descriptor *conf;
   if (libusb_get_active_config_descriptor(dev, &conf) < 0)
      return false;

   bool is_audio = false;

   if (find_interface_class_index(conf, USB_CLASS_AUDIO,
            USB_SUBCLASS_AUDIO_STREAMING) >= 0)
      is_audio = true;

   libusb_free_config_descriptor(conf);
   return is_audio;
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

static bool enumerate_audio_devices(libusb_context *ctx,
      libusb_device **list, unsigned devices,
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

   if (!enumerate_audio_devices(ctx, list, devices, audio_list, num_devices))
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

   if (!poll_list_add(&ctx->fds, ctx->quit_fd[0], POLLIN))
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

static void *thread_entry(void *data)
{
   maru_context *ctx = data;
   (void)ctx;

   pthread_exit(NULL);
}

static bool add_stream(maru_context *ctx, unsigned stream_ep, unsigned feedback_ep)
{
   ctx->streams = realloc(ctx->streams, (ctx->num_streams + 1) * sizeof(*ctx->streams));
   if (!ctx->streams)
      return false;

   ctx->streams[ctx->num_streams].stream_ep = stream_ep;
   ctx->streams[ctx->num_streams].feedback_ep = feedback_ep;

   ctx->num_streams++;
   return true;
}

static void deinit_stream(maru_context *ctx, maru_stream stream)
{
   struct maru_stream_internal *str = &ctx->streams[stream];

   ctx_lock(ctx);

   if (!str->busy)
      goto end;

   if (str->fifo)
   {
      poll_list_remove(&ctx->fds,
            maru_fifo_read_notify_fd(str->fifo));
   }

   maru_fifo_free(str->fifo);
   str->fifo = NULL;
   str->busy = false;

end:
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
   bool ret = true;
   struct libusb_config_descriptor *conf;
   if (libusb_get_active_config_descriptor(libusb_get_device(ctx->handle),
            &conf) < 0)
      return false;

   int ctrl_index = find_interface_class_index(conf,
         USB_CLASS_AUDIO, USB_SUBCLASS_AUDIO_CONTROL);
   int stream_index = find_interface_class_index(conf,
         USB_CLASS_AUDIO, USB_SUBCLASS_AUDIO_STREAMING);

   if (ctrl_index < 0 || stream_index < 0)
   {
      ret = false;
      goto end;
   }

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
         {
            ret = false;
            goto end;
         }
      }

      if (libusb_claim_interface(ctx->handle, ifaces[i]) < 0)
      {
         ret = false;
         goto end;
      }
   }

   if (!enumerate_endpoints(ctx, conf))
   {
      ret = false;
      goto end;
   }

end:
   libusb_free_config_descriptor(conf);
   return ret;
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

   if (!device_is_audio_class(libusb_get_device(context->handle)))
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

   free(ctx);
}

int maru_get_num_streams(maru_context *ctx)
{
   return ctx->num_streams;
}

