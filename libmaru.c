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
#include <fcntl.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>

struct maru_transfer
{
   struct libusb_transfer *trans;
   struct maru_stream_internal *stream;
   struct maru_fifo_locked_region region;

   bool active;
   bool block;

   size_t embedded_data_capacity;
   uint8_t embedded_data[];
};

struct transfer_list
{
   struct maru_transfer **transfers;
   size_t capacity;
   size_t size;
};

struct maru_stream_internal
{
   maru_fifo *fifo;
   unsigned stream_ep;
   unsigned feedback_ep;

   uint32_t transfer_speed;
   uint32_t transfer_speed_fraction;
   unsigned transfer_speed_mult;

   maru_notification_cb write_cb;
   void *write_userdata;
   maru_notification_cb error_cb;
   void *error_userdata;

   struct transfer_list trans;
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
   int quit_fd;
   int notify_fd;
   int volume_fd[2];
   uint64_t volume_count;

   int epfd;

   pthread_mutex_t lock;
   pthread_t thread;
   bool thread_dead;
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

#define USB_AUDIO_FEEDBACK_SIZE        3

#define USB_REQUEST_UAC_SET_CUR        0x01
#define USB_REQUEST_UAC_GET_CUR        0x81
#define USB_REQUEST_UAC_GET_MIN        0x82
#define USB_REQUEST_UAC_GET_MAX        0x83
#define USB_REQUEST_DIR_MASK           0x80

struct maru_volume_request
{
   uint64_t count;
   uint8_t request;
   maru_volume_t volume;
   maru_error error;
   int reply_fd;
};

struct maru_volume_request_buf
{
   struct maru_volume_request req;
   uint8_t buffer[sizeof(struct libusb_control_setup) + sizeof(int16_t)];
};

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

static bool poll_list_add(int epfd, int fd, short events)
{
   struct epoll_event event = {
      .events =
         (events & POLLIN ? EPOLLIN : 0) |
         (events & POLLOUT ? EPOLLOUT : 0),
      .data = {
         .fd = fd,
      },
   };

   return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) == 0;
}

static bool poll_list_remove(int epfd, int fd)
{
   return epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) == 0;
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
   poll_list_add(ctx->epfd, fd, events);
   ctx_unlock(ctx);
}

static void poll_removed_cb(int fd, void *userdata)
{
   maru_context *ctx = userdata;
   ctx_lock(ctx);
   poll_list_remove(ctx->epfd, fd);
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
      if (!poll_list_add(ctx->epfd, fd->fd, fd->events))
      {
         ret = false;
         goto end;
      }
      tmp++;
   }

   if (!poll_list_add(ctx->epfd, ctx->quit_fd, POLLIN))
   {
      ret = false;
      goto end;
   }

   if (!poll_list_add(ctx->epfd, ctx->volume_fd[0], POLLIN))
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

   if (ctx->epfd >= 0)
   {
      close(ctx->epfd);
      ctx->epfd = -1;
   }
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

static void free_transfers_stream(maru_context *ctx,
      struct maru_stream_internal *stream);

static struct maru_transfer *find_vacant_transfer(struct maru_transfer **transfers,
      size_t length, size_t required_buffer)
{
   for (size_t i = 0; i < length; i++)
   {
      if (!transfers[i]->active && transfers[i]->embedded_data_capacity >= required_buffer)
         return transfers[i];
   }

   return NULL;
}

static bool append_transfer(struct transfer_list *list, struct maru_transfer *trans)
{
   if (list->size >= list->capacity)
   {
      size_t new_capacity = list->capacity * 2 + 1;
      struct maru_transfer **new_trans = realloc(list->transfers, new_capacity * sizeof(trans));
      if (!new_trans)
         return false;

      list->capacity = new_capacity;
      list->transfers = new_trans;
   }

   list->transfers[list->size++] = trans;
   return true;
}

static struct maru_transfer *create_transfer(struct transfer_list *list, size_t required_buffer)
{
   struct maru_transfer *trans = calloc(1, sizeof(*trans) + required_buffer);
   if (!trans)
      return NULL;

   trans->embedded_data_capacity = required_buffer;
   trans->trans = libusb_alloc_transfer(1);
   if (!trans->trans)
      goto error;

   if (!append_transfer(list, trans))
      goto error;

   return trans;

error:
   free(trans);
   return NULL;
}

static void transfer_stream_cb(struct libusb_transfer *trans)
{
   struct maru_transfer *transfer = trans->user_data;
   transfer->active = false;

   if (trans->status == LIBUSB_TRANSFER_CANCELLED)
      return;

   if (transfer->stream->fifo && maru_fifo_read_unlock(transfer->stream->fifo, &transfer->region) != LIBMARU_SUCCESS)
      fprintf(stderr, "Stream callback: Failed to unlock fifo!\n");

   maru_notification_cb cb = transfer->stream->write_cb;
   void *userdata = transfer->stream->write_userdata;
   if (cb)
      cb(userdata);

   if (trans->status != LIBUSB_TRANSFER_COMPLETED)
      fprintf(stderr, "Stream callback: Failed transfer ...\n");
}

static void transfer_feedback_cb(struct libusb_transfer *trans)
{
   struct maru_transfer *transfer = trans->user_data;

   if (trans->status == LIBUSB_TRANSFER_CANCELLED)
      transfer->active = false;
   else if (transfer->block)
   {
      transfer->active = false;
      transfer->block = false;
   }

   if (!transfer->active)
      return;

   if (trans->status == LIBUSB_TRANSFER_COMPLETED)
   {
      // TODO: Verify how this really works. Seems like voodoo magic at first glance.
      uint32_t fraction = 
         (trans->buffer[0] << 16) |
         (trans->buffer[1] <<  8) |
         (trans->buffer[2] <<  0);

      fraction >>= 2;

      transfer->stream->transfer_speed_fraction =
         transfer->stream->transfer_speed = fraction;
      //////////
   }

   if (libusb_submit_transfer(trans) < 0)
      fprintf(stderr, "Resubmitting feedback transfer failed ...\n");
}

static void fill_transfer(maru_context *ctx,
      struct maru_transfer *trans, const struct maru_fifo_locked_region *region)
{
   libusb_fill_iso_transfer(trans->trans,
         ctx->handle,
         trans->stream->stream_ep,

         // If we're contigous in ring buffer, we can just read directly from it.
         region->second ? trans->embedded_data : region->first,

         region->first_size + region->second_size,
         1,
         transfer_stream_cb,
         trans,
         1000);

   libusb_set_iso_packet_lengths(trans->trans, region->first_size + region->second_size);

   if (region->second)
   {
      memcpy(trans->embedded_data, region->first, region->first_size);
      memcpy(trans->embedded_data + region->first_size, region->second, region->second_size);
   }

   trans->region = *region;
}

static bool enqueue_transfer(maru_context *ctx, struct maru_stream_internal *stream,
      const struct maru_fifo_locked_region *region)
{
   // If our region is split, we have to make a copy to get a contigous transfer.
   size_t required_buffer = region->second_size ? region->first_size + region->second_size : 0;

   // If we can reap old, used transfers, we'll reuse them as-is, no need to reallocate
   // transfers all the time. Eventually, given a fixed sized fifo buffer,
   // no new transfer will have to be
   // allocated, as there is always a vacant one.
   struct maru_transfer *transfer = find_vacant_transfer(stream->trans.transfers,
         stream->trans.size, required_buffer);

   if (!transfer)
      transfer = create_transfer(&stream->trans, required_buffer);

   if (!transfer)
      return NULL;

   transfer->stream = stream;
   transfer->active = true;
   fill_transfer(ctx, transfer, region);

   if (libusb_submit_transfer(transfer->trans) < 0)
   {
      transfer->active = false;
      return false;
   }

   return true;
}

static bool enqueue_feedback_transfer(maru_context *ctx, struct maru_stream_internal *stream)
{
   struct maru_transfer *trans = calloc(1, sizeof(*trans) + USB_AUDIO_FEEDBACK_SIZE);
   if (!trans)
      return NULL;

   trans->embedded_data_capacity = USB_AUDIO_FEEDBACK_SIZE;

   trans->trans = libusb_alloc_transfer(1);
   if (!trans->trans)
      goto error;

   libusb_fill_iso_transfer(trans->trans,
         ctx->handle,
         stream->feedback_ep,
         trans->embedded_data,
         USB_AUDIO_FEEDBACK_SIZE,
         1, transfer_feedback_cb, trans, 1000);

   libusb_set_iso_packet_lengths(trans->trans, USB_AUDIO_FEEDBACK_SIZE);

   trans->stream = stream;
   trans->active = true;

   if (!append_transfer(&stream->trans, trans))
      goto error;

   if (libusb_submit_transfer(trans->trans) < 0)
   {
      trans->active = false;
      return false;
   }

   return true;

error:
   if (trans)
   {
      libusb_free_transfer(trans->trans);
      free(trans);
   }
   return false;
}

static size_t stream_chunk_size(struct maru_stream_internal *stream)
{
   // Calculate fractional speeds (async isochronous).
   stream->transfer_speed_fraction += stream->transfer_speed & 0xffff;

   size_t to_write = stream->transfer_speed_fraction >> 16;
   to_write *= stream->transfer_speed_mult;

   // Wrap-around.
   stream->transfer_speed_fraction = (UINT32_C(0xffff0000) & stream->transfer_speed)
      | (stream->transfer_speed_fraction & 0xffff);

   return to_write;
}

static void handle_stream(maru_context *ctx, struct maru_stream_internal *stream)
{
   ctx_lock(ctx);

   // It is possible that an open stream was suddenly closed.
   // If so, we can catch it here and ignore it.
   if (!stream->fifo)
   {
      free_transfers_stream(ctx, stream);
      goto end;
   }

   size_t avail = maru_fifo_read_avail(stream->fifo);
   size_t to_write = stream_chunk_size(stream);

   if (avail >= to_write)
   {
      struct maru_fifo_locked_region region;
      maru_fifo_read_lock(stream->fifo, to_write,
            &region);

      if (!enqueue_transfer(ctx, stream, &region))
         fprintf(stderr, "Enqueue transfer failed!\n");
   }

   if (maru_fifo_read_notify_ack(stream->fifo) != LIBMARU_SUCCESS)
   {
      free_transfers_stream(ctx, stream);
      epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, maru_fifo_read_notify_fd(stream->fifo), NULL);
   }

end:
   ctx_unlock(ctx);
}

static void free_transfers_stream(maru_context *ctx,
      struct maru_stream_internal *stream)
{
   struct transfer_list *list = &stream->trans;

   for (unsigned trans = 0; trans < list->size; trans++)
   {
      struct maru_transfer *transfer = list->transfers[trans];

      // We have to cancel the stream, and wait for it to complete.
      // Cancellation is async as well.
      if (transfer->active)
      {
         transfer->block = true;
         libusb_cancel_transfer(transfer->trans);
         while (transfer->active)
            libusb_handle_events(ctx->ctx);
      }

      libusb_free_transfer(transfer->trans);
      free(transfer);
   }

   free(list->transfers);
   memset(list, 0, sizeof(*list));
}

static void free_transfers(maru_context *ctx)
{
   for (unsigned str = 0; str < ctx->num_streams; str++)
      free_transfers_stream(ctx, &ctx->streams[str]);
}

static void kill_write_notifications(maru_context *ctx)
{
   ctx_lock(ctx);

   for (unsigned i = 0; i < ctx->num_streams; i++)
   {
      if (ctx->streams[i].fifo)
         maru_fifo_kill_notification(ctx->streams[i].fifo);
   }

   // We are now officially dead, and no more streams or writes can be performed.
   ctx->thread_dead = true;
   ctx_unlock(ctx);
}

static void transfer_control_cb(struct libusb_transfer *trans)
{
   struct maru_volume_request_buf *buf = trans->user_data;

   if (buf->req.request & USB_REQUEST_DIR_MASK)
   {
      uint16_t val;
      memcpy(&val, libusb_control_transfer_get_data(trans), sizeof(val));
      buf->req.volume = (maru_volume_t)libusb_le16_to_cpu(val);
   }

   switch (trans->status)
   {
      case LIBUSB_TRANSFER_COMPLETED:
         buf->req.error = LIBMARU_SUCCESS;
         break;
         
      case LIBUSB_TRANSFER_TIMED_OUT:
         buf->req.error = LIBMARU_ERROR_TIMEOUT;
         break;

      default:
         buf->req.error = LIBMARU_ERROR_IO;
         break;
   }

   write(buf->req.reply_fd, &buf->req, sizeof(buf->req));

   free(buf);
   libusb_free_transfer(trans);
}

static void handle_volume(maru_context *ctx,
      int fd)
{
   struct maru_volume_request req;
   if (read(fd, &req, sizeof(req)) != (ssize_t)sizeof(req))
      return;

   struct libusb_transfer *trans = libusb_alloc_transfer(0);
   if (!trans)
   {
      req.error = LIBMARU_ERROR_MEMORY;
      write(req.reply_fd, &req, sizeof(req));
      return;
   }

   struct maru_volume_request_buf *buf = calloc(1, sizeof(*buf));
   if (!buf)
   {
      req.error = LIBMARU_ERROR_MEMORY;
      write(req.reply_fd, &req, sizeof(req));
      return;
   }

   libusb_fill_control_setup(buf->buffer,
         LIBUSB_REQUEST_TYPE_CLASS | (req.request & USB_REQUEST_DIR_MASK),
         req.request,
         2 << 8, // Volume only for now.
         0,
         sizeof(int16_t));

   buf->req = req;
   libusb_fill_control_transfer(trans,
         ctx->handle,
         buf->buffer,
         transfer_control_cb,
         buf,
         5000);

   if (!(req.request & USB_REQUEST_DIR_MASK))
   {
      uint16_t vol = libusb_cpu_to_le16(req.volume);
      memcpy(libusb_control_transfer_get_data(trans), &vol, sizeof(vol));
   }

   if (libusb_submit_transfer(trans) < 0)
   {
      free(buf);
      libusb_free_transfer(trans);

      req.error = LIBMARU_ERROR_IO;
      write(req.reply_fd, &req, sizeof(req));
   }
}

static void *thread_entry(void *data)
{
   maru_context *ctx = data;

   bool alive = true;

   while (alive)
   {
      struct epoll_event events[16];
      int num_events;

poll_retry:
      if ((num_events = epoll_wait(ctx->epfd, events, 16, -1)) < 0)
      {
         if (errno == EINTR)
            goto poll_retry;

         perror("epoll_wait");
         break;
      }

      bool libusb_event = false;

      for (size_t i = 0; i < num_events; i++)
      {
         int fd = events[i].data.fd;
         struct maru_stream_internal *stream = NULL;

         if ((stream = fd_to_stream(ctx, fd)))
            handle_stream(ctx, stream);
         else if (fd == ctx->quit_fd)
            alive = false;
         else if (fd == ctx->volume_fd[0])
            handle_volume(ctx, fd);
         else
            libusb_event = true;
      }

      if (libusb_event &&
            libusb_handle_events_timeout(ctx->ctx, &(struct timeval) {0}) < 0)
      {
         fprintf(stderr, "libusb_handle_events_timeout() failed!\n");
         alive = false;
      }
   }

   free_transfers(ctx);
   kill_write_notifications(ctx);
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

static bool init_stream_nolock(maru_context *ctx,
      maru_stream stream,
      const struct maru_stream_desc *desc)
{
   struct maru_stream_internal *str = &ctx->streams[stream];

   if (!enqueue_feedback_transfer(ctx, str))
      return false;

   size_t buffer_size = desc->buffer_size;
   if (buffer_size == 0)
      buffer_size = 4096;

   size_t frame_size = (desc->sample_rate *
      desc->channels *
      desc->bits / 8) / 1000;

   // Need a sufficiently large buffer to operate somewhat correctly.
   // This works out to rougly 4ms.
   if (buffer_size < 4 * frame_size)
      return false;

   // When POLLIN fires in thread, we want to be able
   // to send a full frame to libusb directly from the buffer.
   // Frame sizes might vary slighly over time (async isochronous),
   // so be safe here and trigger on twice the nominal size.
   size_t trigger_size = frame_size * 2;

   str->fifo = maru_fifo_new(buffer_size);
   if (!str->fifo)
      return false;

   if (maru_fifo_set_read_trigger(str->fifo,
            trigger_size) < 0)
   {
      maru_fifo_free(str->fifo);
      str->fifo = NULL;
      return false;
   }

   poll_list_add(ctx->epfd,
         maru_fifo_read_notify_fd(str->fifo), POLLIN);

   str->transfer_speed_mult = desc->channels * desc->bits / 8;

   str->transfer_speed_fraction = desc->sample_rate;
   str->transfer_speed_fraction <<= 16;
   str->transfer_speed_fraction /= 1000;

   str->transfer_speed = str->transfer_speed_fraction;
   
   return true;
}

static void deinit_stream_nolock(maru_context *ctx, maru_stream stream)
{
   struct maru_stream_internal *str = &ctx->streams[stream];

   if (str->fifo)
   {
      poll_list_remove(ctx->epfd,
            maru_fifo_read_notify_fd(str->fifo));

      maru_fifo_free(str->fifo);
      str->fifo = NULL;
   }
}

static void deinit_stream(maru_context *ctx, maru_stream stream)
{
   ctx_lock(ctx);
   deinit_stream_nolock(ctx, stream);
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

   context->quit_fd = eventfd(0, 0);
   context->epfd = epoll_create(16);
   if (socketpair(AF_UNIX, SOCK_STREAM, 0, context->volume_fd) < 0)
      goto error;

   fprintf(stderr, "Created socketpair: %d - %d\n",
         context->volume_fd[0], context->volume_fd[1]);

   if (fcntl(context->volume_fd[0], F_SETFL,
            fcntl(context->volume_fd[0], F_GETFL) | O_NONBLOCK) < 0)
      goto error;
   if (fcntl(context->volume_fd[1], F_SETFL,
            fcntl(context->volume_fd[1], F_GETFL) | O_NONBLOCK) < 0)
      goto error;

   if (context->quit_fd < 0 ||
         context->epfd < 0)
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

   if (ctx->quit_fd >= 0)
   {
      eventfd_write(ctx->quit_fd, 1);
      if (ctx->thread)
         pthread_join(ctx->thread, NULL);
      close(ctx->quit_fd);
   }

   if (ctx->volume_fd[0] >= 0)
      close(ctx->volume_fd[0]);
   if (ctx->volume_fd[1] >= 0)
      close(ctx->volume_fd[1]);

   poll_list_deinit(ctx);

   for (unsigned i = 0; i < ctx->num_streams; i++)
      deinit_stream(ctx, i);
   free(ctx->streams);

   pthread_mutex_destroy(&ctx->lock);

   if (ctx->conf)
      libusb_free_config_descriptor(ctx->conf);

   if (ctx->handle)
      libusb_close(ctx->handle);

   if (ctx->ctx)
      libusb_exit(ctx->ctx);

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

static int maru_is_stream_available_nolock(maru_context *ctx,
      maru_stream stream)
{
   return stream < ctx->num_streams ? 
      !ctx->streams[stream].fifo :
      LIBMARU_ERROR_INVALID;
}

int maru_is_stream_available(maru_context *ctx, maru_stream stream)
{
   ctx_lock(ctx);
   int ret = maru_is_stream_available_nolock(ctx, stream);
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

maru_error maru_stream_open(maru_context *ctx,
      maru_stream stream,
      const struct maru_stream_desc *desc)
{
   maru_error ret = LIBMARU_SUCCESS;
   ctx_lock(ctx);

   if (ctx->thread_dead)
   {
      ret = LIBMARU_ERROR_BUSY;
      goto end;
   }

   if (stream >= ctx->num_streams)
   {
      ret = LIBMARU_ERROR_INVALID;
      goto end;
   }

   if (maru_is_stream_available_nolock(ctx, stream) == 0)
   {
      ret = LIBMARU_ERROR_BUSY;
      goto end;
   }

   if (!init_stream_nolock(ctx, stream, desc))
   {
      ret = LIBMARU_ERROR_GENERIC;
      goto end;
   }

end:
   ctx_unlock(ctx);
   return ret;
}

maru_error maru_stream_close(maru_context *ctx,
      maru_stream stream)
{
   maru_error ret = LIBMARU_SUCCESS;
   ctx_lock(ctx);

   if (maru_is_stream_available_nolock(ctx, stream) != 0)
   {
      ret = LIBMARU_ERROR_INVALID;
      goto end;
   }

   deinit_stream_nolock(ctx, stream);

end:
   ctx_unlock(ctx);
   return ret;
}

size_t maru_stream_write(maru_context *ctx, maru_stream stream,
      const void *data, size_t size)
{
   if (stream >= ctx->num_streams)
      return 0;

   maru_fifo *fifo = ctx->streams[stream].fifo;
   if (!fifo)
   {
      fprintf(stderr, "Stream has no fifo!\n");
      return 0;
   }

   return maru_fifo_blocking_write(fifo, data, size);
}

size_t maru_stream_write_avail(maru_context *ctx, maru_stream stream)
{
   if (stream >= ctx->num_streams)
      return 0;

   maru_fifo *fifo = ctx->streams[stream].fifo;
   if (!fifo)
      return 0;

   return maru_fifo_write_avail(fifo);
}

static maru_error perform_request(maru_context *ctx,
      maru_volume_t *vol, uint8_t request, maru_usec timeout)
{
   const struct maru_volume_request req = {
      .count = ctx->volume_count++,
      .request = request,
      .volume = *vol,
      .reply_fd = ctx->volume_fd[0],
   };

   if (write(ctx->volume_fd[1], &req, sizeof(req)) != (ssize_t)sizeof(req))
      return LIBMARU_ERROR_IO;

   struct maru_volume_request ret_req;

   do
   {
      // Wait for reply from thread.
      struct pollfd fds = {
         .fd = ctx->volume_fd[1],
         .events = POLLIN,
      };

poll_retry:
      if (poll(&fds, 1, timeout < 0 ? -1 : timeout / 1000) < 0)
      {
         if (errno == EINTR)
            goto poll_retry;

         return LIBMARU_ERROR_IO;
      }

      if (fds.revents & (POLLHUP | POLLERR | POLLNVAL))
         return LIBMARU_ERROR_IO;

      if (!(fds.revents & POLLIN))
         return LIBMARU_ERROR_TIMEOUT;

      if (read(ctx->volume_fd[1], &ret_req, sizeof(ret_req)) != (ssize_t)sizeof(ret_req))
         return LIBMARU_ERROR_IO;

   } while (ret_req.count != req.count);

   *vol = ret_req.volume;
   return ret_req.error;
}

maru_error maru_stream_get_volume(maru_context *ctx,
      maru_stream stream,
      maru_volume_t *current, maru_volume_t *min, maru_volume_t *max,
      maru_usec timeout)
{
   // Only support master channel volume for now.
   if (stream != LIBMARU_STREAM_MASTER)
      return LIBMARU_ERROR_INVALID;

   if (current)
   {
      maru_error err = perform_request(ctx, current,
            USB_REQUEST_UAC_GET_CUR, timeout);

      if (err != LIBMARU_SUCCESS)
         return err;
   }

   if (min)
   {
      maru_error err = perform_request(ctx, min,
            USB_REQUEST_UAC_GET_MIN, timeout);

      if (err != LIBMARU_SUCCESS)
         return err;
   }

   if (max)
   {
      maru_error err = perform_request(ctx, max,
            USB_REQUEST_UAC_GET_MAX, timeout);

      if (err != LIBMARU_SUCCESS)
         return err;
   }

   return LIBMARU_SUCCESS;
}

maru_error maru_stream_set_volume(maru_context *ctx,
      maru_stream stream,
      maru_volume_t volume,
      maru_usec timeout)
{
   // Only support master channel volume for now.
   if (stream != LIBMARU_STREAM_MASTER)
      return LIBMARU_ERROR_INVALID;

   return perform_request(ctx, &volume, USB_REQUEST_UAC_SET_CUR, timeout);
}

