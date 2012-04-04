/* libmaru - Userspace USB audio class driver.
 * Copyright (C) 2012 - Hans-Kristian Arntzen
 * Copyright (C) 2012 - Agnes Heyer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

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
#include <time.h>

/** \ingroup lib
 * \brief Struct holding information needed for a libusb transfer. */
struct maru_transfer
{
   /** Underlying libusb transfer struct */
   struct libusb_transfer *trans;

   /** Associated context */
   maru_context *ctx;

   /** Associated stream */
   struct maru_stream_internal *stream;

   /** Associated fifo region */
   struct maru_fifo_locked_region region;

   /** Set if the transfer is currently queued up in libusb's event system. */
   bool active;
   /** Set if transfer should not be queued up again.
    * Used mostly for feedback endpoint. */
   bool block;

   /** Capacity of embedded_data */
   size_t embedded_data_capacity;
   /** Embeddable structure for use to transfer data that
    * cannot be transfered contigously in region. */
   uint8_t embedded_data[];
};

/** \ingroup lib
 * \brief Struct holding a list of active and vacant transfers for a stream. */
struct transfer_list
{
   /** Vector of allocated transfers. */
   struct maru_transfer **transfers;
   /** Capacity of transfers. */
   size_t capacity;
   /** Size of transfers. */
   size_t size;
};

/** \ingroup lib
 * \brief Struct holding information needed for a single stream. */
struct maru_stream_internal
{
   /** The associated fifo of a stream. Set to NULL if a stream is not claimed. */
   maru_fifo *fifo;
   /** Associated streaming endpoint of the stream. */
   unsigned stream_ep;
   /** Associated feedback endpoint of the stream. */
   unsigned feedback_ep;

   /** Fixed point transfer speed in audio frames / USB frame (16.16). */
   uint32_t transfer_speed;
   /** Fraction that keeps track of when to send extra frames to keep up with transfer_speed. */
   uint32_t transfer_speed_fraction;
   /** Multiplier for transfer_speed to convert frames to bytes. */
   unsigned transfer_speed_mult;
   /** Bytes-per-second data rate for stream. */
   size_t bps;

   /** Optional callback to notify write avail. */
   maru_notification_cb write_cb;
   /** Userdata for write_cb */
   void *write_userdata;
   /** Optional callbak to notify errors in stream. */
   maru_notification_cb error_cb;
   /** Userdata for error_cb */
   void *error_userdata;

   /** eventfd to synchronize tear-down of a stream. */
   int sync_fd;

   /** Transfer list. */
   struct transfer_list trans;
   /** Transfers currently queued up. */
   unsigned trans_count;
   /** Maximum number of transfer allowed to be queued up. */
   unsigned enqueue_count;

   struct
   {
      /** Set if timer is started, and start_time and offset have valid times. */
      bool started;
      /** Time of timer start. */
      maru_usec start_time;
      /** Timer offset. Used to combat clock skew. */
      maru_usec offset;
      /** Total write count, used along with start_time to calculate latency. */
      uint64_t write_cnt;
   } timer;
};

/** \ingroup lib
 * \brief Struct holding information in the libmaru context. */
struct maru_context
{
   /** Underlying libusb context */
   libusb_context *ctx;
   /** libusb device handle to the audio card */
   libusb_device_handle *handle;
   /** Cached configuration descriptor for audio card */
   struct libusb_config_descriptor *conf;

   /** List of allocated streams */
   struct maru_stream_internal *streams;
   /** Number of allocated hardware streams */
   unsigned num_streams;

   /** Interface index for audio control interface */
   unsigned control_interface;
   /** Interface index for audio streaming interface */
   unsigned stream_interface;
   /** Altsetting for streaming interface */
   unsigned stream_altsetting;

   /** File descriptor that thread polls for to tear down cleanly in maru_destroy_context(). */
   int quit_fd;
   /** Socketpair to communicate control requests into and out of thread. */
   int request_fd[2];
   /** Number of control requests that have been made up to this point.
    * Used to match requests with replies. */
   uint64_t request_count;

   /** epoll descriptor that thread polls on. */
   int epfd;

   /** Context-global lock */
   pthread_mutex_t lock;
   /** Thread */
   pthread_t thread;
   /** Set to true if thread has died prematurely */
   bool thread_dead;

   struct
   {
      /** Number of feature unit channels must be performed requests on */
      unsigned chans;
      /** Feature unit channels */
      unsigned channels[8];
      /** The feature unit that supports volume control for output stream */
      unsigned feature_unit;
   } volume;
};

// __attribute__((packed)) is a GNU extension.

// USB audio spec structures.
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
   uint8_t tSamFreq[];
} __attribute__((packed));

struct usb_uac_output_terminal_descriptor
{
   uint8_t  bLength;
   uint8_t  bDescriptorType;
   uint8_t  bDescriptorSubtype;
   uint8_t  bTerminalID;
   uint16_t wTerminalType;
   uint8_t  bAssocTerminal;
   uint8_t  bSourceID;
   uint8_t  iTerminal;
} __attribute__((packed));

struct usb_uac_feature_unit_descriptor
{
   uint8_t bLength;
   uint8_t bDescriptorType;
   uint8_t bDescriptorSubtype;
   uint8_t bUnitID;
   uint8_t bSourceID;
   uint8_t bControlSize;
   uint8_t bmaControls[];
} __attribute__((packed));

#define USB_CLASS_AUDIO                1
#define USB_SUBCLASS_AUDIO_CONTROL     1
#define USB_SUBCLASS_AUDIO_STREAMING   2

#define USB_ENDPOINT_ISOCHRONOUS       0x01
#define USB_ENDPOINT_ASYNC             0x04
#define USB_ENDPOINT_ADAPTIVE          0x08

#define USB_CLASS_DESCRIPTOR           0x20
#define USB_INTERFACE_DESCRIPTOR_TYPE  0x04
#define USB_FORMAT_DESCRIPTOR_SUBTYPE  0x02
#define USB_FORMAT_TYPE_I              0x01
#define USB_FREQ_TYPE_DIRECT           0x01
#define USB_FREQ_TYPE_DISCRETE         0x02

#define USB_AUDIO_FEEDBACK_SIZE        3
#define USB_MAX_CONTROL_SIZE           64

#define USB_REQUEST_UAC_SET_CUR        0x01
#define USB_REQUEST_UAC_GET_CUR        0x81
#define USB_REQUEST_UAC_GET_MIN        0x82
#define USB_REQUEST_UAC_GET_MAX        0x83
#define USB_UAC_VOLUME_SELECTOR        0x02
#define UAS_FREQ_CONTROL               0x01
#define UAS_PITCH_CONTROL              0x02
#define USB_REQUEST_DIR_MASK           0x80
#define UAC_TYPE_SPEAKER               0x0301
#define UAC_OUTPUT_TERMINAL            0x03
#define UAC_FEATURE_UNIT               0x06

/** \ingroup lib
 * \brief Struct holding data for a USB control request.
 */
struct maru_control_request
{
   /** Serialization count */
   uint64_t count;

   /** USB control request type */
   uint8_t request_type;
   /** USB control request */
   uint8_t request;
   /** USB control value */
   uint16_t value;
   /** USB control index */
   uint16_t index;

   struct
   {
      /** Setup packet */
      uint8_t setup[sizeof(struct libusb_control_setup)];
      /** Optional payload */
      uint8_t data[USB_MAX_CONTROL_SIZE];
   } __attribute__((packed)) data;

   /** Size of transfer (wLength) */
   size_t size;

   /** Error code */
   maru_error error;
   /** Descriptor thread will reply on after finishing transfer */
   int reply_fd;
};

static int find_interface_class_index(const struct libusb_config_descriptor *conf,
      unsigned class, unsigned subclass,
      unsigned min_eps, int *altsetting)
{
   for (unsigned i = 0; i < conf->bNumInterfaces; i++)
   {
      for (int j = 0; j < conf->interface[i].num_altsetting; j++)
      {
         const struct libusb_interface_descriptor *desc =
            &conf->interface[i].altsetting[j];

         if (desc->bInterfaceClass == class &&
               desc->bInterfaceSubClass == subclass &&
               desc->bNumEndpoints >= min_eps)
         {
            if (altsetting)
               *altsetting = j;

            return i;
         }
      }
   }

   return -1;
}

static bool parse_audio_format(const uint8_t *data, size_t size, struct maru_stream_desc *desc);

static int perform_pitch_request(maru_context *ctx,
      unsigned ep,
      maru_usec timeout);

static maru_error perform_rate_request(maru_context *ctx,
      unsigned ep,
      unsigned rate,
      maru_usec timeout);

static bool format_matches(const struct libusb_interface_descriptor *iface,
      const struct maru_stream_desc *desc)
{
   struct maru_stream_desc format_desc;

   if (!parse_audio_format(iface->extra, iface->extra_length, &format_desc))
      return false;

   if (!desc)
      return true;

   if (desc->sample_rate && desc->sample_rate != format_desc.sample_rate)
      return false;
   if (desc->channels && desc->channels != format_desc.channels)
      return false;
   if (desc->bits && desc->bits != format_desc.bits)
      return false;

   return true;
}

static int find_stream_interface_alt(const struct libusb_config_descriptor *conf,
      const struct maru_stream_desc *format_desc, int *altsetting)
{
   for (unsigned i = 0; i < conf->bNumInterfaces; i++)
   {
      for (int j = 0; j < conf->interface[i].num_altsetting; j++)
      {
         const struct libusb_interface_descriptor *desc =
            &conf->interface[i].altsetting[j];

         if (desc->bInterfaceClass == USB_CLASS_AUDIO &&
               desc->bInterfaceSubClass == USB_SUBCLASS_AUDIO_STREAMING &&
               desc->bNumEndpoints >= 1 &&
               format_matches(desc, format_desc))
         {
            if (altsetting)
               *altsetting = j;

            return i;
         }
      }
   }

   return -1;
}

static bool conf_is_audio_class(const struct libusb_config_descriptor *conf)
{
   return find_interface_class_index(conf, USB_CLASS_AUDIO,
         USB_SUBCLASS_AUDIO_STREAMING, 1, NULL) >= 0;
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
   libusb_exit(ctx);
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

static void poll_list_unblock(int epfd, int fd, short events)
{
   struct epoll_event event = {
      .events =
         (events & POLLIN ? EPOLLIN : 0) |
         (events & POLLOUT ? EPOLLOUT : 0),
      .data = {
         .fd = fd,
      },
   };

   if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event) < 0)
   {
      fprintf(stderr, "poll_list_unblock() failed!\n");
      perror("epoll_ctl");
   }
}

static void poll_list_block(int epfd, int fd)
{
   struct epoll_event event = { .events = 0, .data = { .fd = fd } };
   if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event) < 0)
   {
      fprintf(stderr, "poll_list_block() failed!\n");
      perror("epoll_ctl");
   }
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
   poll_list_add(ctx->epfd, fd, events);
}

static void poll_removed_cb(int fd, void *userdata)
{
   maru_context *ctx = userdata;
   poll_list_remove(ctx->epfd, fd);
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

   if (!poll_list_add(ctx->epfd, ctx->request_fd[0], POLLIN))
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

   for (unsigned i = 0; i < ctx->num_streams; i++)
   {
      if (ctx->streams[i].fifo &&
            fd == maru_fifo_read_notify_fd(ctx->streams[i].fifo))
      {
         ret = &ctx->streams[i];
         break;
      }
   }

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

#define LIBMARU_MAX_ENQUEUE_COUNT 32
#define LIBMARU_MAX_ENQUEUE_TRANSFERS 4

static struct maru_transfer *create_transfer(struct transfer_list *list, size_t required_buffer)
{
   struct maru_transfer *trans = calloc(1, sizeof(*trans) + required_buffer);
   if (!trans)
      return NULL;

   trans->embedded_data_capacity = required_buffer;
   trans->trans = libusb_alloc_transfer(LIBMARU_MAX_ENQUEUE_COUNT);
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

   transfer->stream->trans_count--;

   // If we are deiniting, we will die before this can be used.
   if (!transfer->block)
   {
      poll_list_unblock(transfer->ctx->epfd,
            maru_fifo_read_notify_fd(transfer->stream->fifo),
            POLLIN);

      if (maru_fifo_read_unlock(transfer->stream->fifo, &transfer->region) != LIBMARU_SUCCESS)
         fprintf(stderr, "Error occured during read unlock!\n");
   }

   if (trans->status == LIBUSB_TRANSFER_CANCELLED)
      return;

   maru_notification_cb cb = transfer->stream->write_cb;
   void *userdata = transfer->stream->write_userdata;
   if (cb)
      cb(userdata);

   for (int i = 0; i < trans->num_iso_packets; i++)
   {
      if (trans->iso_packet_desc[i].length != trans->iso_packet_desc[i].actual_length)
         fprintf(stderr, "Actual length differs from sent length! (Actual: %d, Requested: %d)\n",
               trans->iso_packet_desc[i].actual_length,
               trans->iso_packet_desc[i].length);
   }

   if (trans->status != LIBUSB_TRANSFER_COMPLETED)
      fprintf(stderr, "Stream callback: Failed transfer ... (status: %d)\n", trans->status);
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
      struct maru_transfer *trans, const struct maru_fifo_locked_region *region,
      const unsigned *packet_len, unsigned packets)
{
   libusb_fill_iso_transfer(trans->trans,
         ctx->handle,
         trans->stream->stream_ep,

         // If we're contigous in ring buffer, we can just read directly from it.
         region->second ? trans->embedded_data : region->first,

         region->first_size + region->second_size,
         packets,
         transfer_stream_cb,
         trans,
         1000);

   for (unsigned i = 0; i < packets; i++)
      trans->trans->iso_packet_desc[i].length = packet_len[i];

   if (region->second)
   {
      memcpy(trans->embedded_data, region->first, region->first_size);
      memcpy(trans->embedded_data + region->first_size, region->second, region->second_size);
   }

   trans->region = *region;
}

static bool enqueue_transfer(maru_context *ctx, struct maru_stream_internal *stream,
      const struct maru_fifo_locked_region *region, const unsigned *packet_len, unsigned packets)
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
      return false;

   transfer->stream = stream;
   transfer->ctx    = ctx;
   transfer->active = true;

   fill_transfer(ctx, transfer, region, packet_len, packets);

   if (libusb_submit_transfer(transfer->trans) < 0)
   {
      transfer->active = false;
      return false;
   }

   stream->trans_count++;
   if (stream->trans_count >= LIBMARU_MAX_ENQUEUE_TRANSFERS && stream->fifo)
      poll_list_block(ctx->epfd, maru_fifo_read_notify_fd(stream->fifo));

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
   size_t new_fraction = stream->transfer_speed_fraction + (stream->transfer_speed & 0xffff);

   size_t to_write = new_fraction >> 16;
   to_write *= stream->transfer_speed_mult;

   return to_write;
}

static void stream_chunk_size_finalize(struct maru_stream_internal *stream)
{
   // Calculate fractional speeds (async isochronous).
   stream->transfer_speed_fraction += stream->transfer_speed & 0xffff;

   // Wrap-around.
   stream->transfer_speed_fraction = (UINT32_C(0xffff0000) & stream->transfer_speed)
      | (stream->transfer_speed_fraction & 0xffff);
}

static void handle_stream(maru_context *ctx, struct maru_stream_internal *stream)
{
   size_t avail = maru_fifo_read_avail(stream->fifo);

   unsigned packet_len[LIBMARU_MAX_ENQUEUE_COUNT];
   unsigned packets = 0;
   size_t total_write = 0;

   size_t to_write = stream_chunk_size(stream);
   while (avail >= to_write && packets < stream->enqueue_count)
   {
      total_write += to_write;
      packet_len[packets++] = to_write;
      avail -= to_write;
      stream_chunk_size_finalize(stream);
      to_write = stream_chunk_size(stream);
   }

   if (packets)
   {
      struct maru_fifo_locked_region region;
      maru_fifo_read_lock(stream->fifo, total_write,
            &region);

      if (!enqueue_transfer(ctx, stream, &region, packet_len, packets))
         fprintf(stderr, "Enqueue transfer failed!\n");
   }

   // We are being killed, kill all transfers and tell other thread it's safe to deinit.
   if (maru_fifo_read_notify_ack(stream->fifo) != LIBMARU_SUCCESS)
   {
      free_transfers_stream(ctx, stream);
      epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, maru_fifo_read_notify_fd(stream->fifo), NULL);
      eventfd_write(stream->sync_fd, 1);
   }
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
   struct maru_control_request *buf = trans->user_data;

   switch (trans->status)
   {
      case LIBUSB_TRANSFER_COMPLETED:
         buf->error = LIBMARU_SUCCESS;
         break;
         
      case LIBUSB_TRANSFER_TIMED_OUT:
         buf->error = LIBMARU_ERROR_TIMEOUT;
         break;

      case LIBUSB_TRANSFER_STALL:
      {
         int ret;
         buf->error = LIBMARU_ERROR_INVALID;
         if ((ret = libusb_clear_halt(trans->dev_handle, trans->endpoint)) < 0)
            fprintf(stderr, "Failed to clear stall (error: %d)!\n", ret);
         break;
      }

      case LIBUSB_TRANSFER_ERROR:
         buf->error = LIBMARU_ERROR_IO;
         fprintf(stderr, "Control transfer failed!\n");
         break;

      default:
         buf->error = LIBMARU_ERROR_IO;
         break;
   }

   write(buf->reply_fd, buf, sizeof(*buf));

   free(buf);
   libusb_free_transfer(trans);
}

static void handle_request(maru_context *ctx,
      int fd)
{
   struct maru_control_request req;
   if (read(fd, &req, sizeof(req)) != (ssize_t)sizeof(req))
      return;

   struct libusb_transfer *trans = libusb_alloc_transfer(0);
   if (!trans)
   {
      req.error = LIBMARU_ERROR_MEMORY;
      write(req.reply_fd, &req, sizeof(req));
      return;
   }

   struct maru_control_request *buf = calloc(1, sizeof(*buf));
   if (!buf)
   {
      req.error = LIBMARU_ERROR_MEMORY;
      write(req.reply_fd, &req, sizeof(req));
      return;
   }

   *buf = req;

   req.request_type |= req.request & USB_REQUEST_DIR_MASK;

   libusb_fill_control_setup(buf->data.setup,
         req.request_type,
         req.request,
         req.value,
         req.index,
         req.size);

#if 0
   fprintf(stderr, "Request:\n");
   fprintf(stderr, "\tRequest Type: 0x%02x\n", (unsigned)req.request_type);
   fprintf(stderr, "\tRequest:      0x%02x\n", (unsigned)req.request);
   fprintf(stderr, "\tValue:        0x%04x\n", (unsigned)req.value);
   fprintf(stderr, "\tIndex:        0x%04x\n", (unsigned)req.index);
   fprintf(stderr, "\tSize:         0x%04x\n", (unsigned)req.size);
#endif

   libusb_fill_control_transfer(trans,
         ctx->handle,
         buf->data.setup,
         transfer_control_cb,
         buf,
         1000);

   if (libusb_submit_transfer(trans) < 0)
   {
      fprintf(stderr, "Submit transfer failed ...\n");
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

#define MAX_EVENTS 16
   while (alive)
   {
      struct epoll_event events[MAX_EVENTS];
      int num_events;

      if ((num_events = epoll_wait(ctx->epfd, events, MAX_EVENTS, -1)) < 0)
      {
         if (errno == EINTR)
            continue;

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
         else if (fd == ctx->request_fd[0])
            handle_request(ctx, fd);
         else
            libusb_event = true;
      }

      if (libusb_event)
      {
         if (libusb_handle_events_timeout(ctx->ctx, &(struct timeval) {0}) < 0)
         {
            fprintf(stderr, "libusb_handle_events_timeout() failed!\n");
            alive = false;
         }
      }
   }

   free_transfers(ctx);
   kill_write_notifications(ctx);
   return NULL;
}

static bool add_stream(maru_context *ctx, unsigned stream_ep, unsigned feedback_ep)
{
   struct maru_stream_internal *new_streams = realloc(ctx->streams, (ctx->num_streams + 1) * sizeof(*ctx->streams));
   if (!new_streams)
      return false;

   ctx->streams = new_streams;

   ctx->streams[ctx->num_streams] = (struct maru_stream_internal) {
      .stream_ep = stream_ep,
      .feedback_ep = feedback_ep,
      .sync_fd = -1,
   };

   ctx->num_streams++;
   return true;
}

static bool init_stream_nolock(maru_context *ctx,
      maru_stream stream,
      const struct maru_stream_desc *desc)
{
   struct maru_stream_internal *str = &ctx->streams[stream];

   str->sync_fd = eventfd(0, 0);
   if (str->sync_fd < 0)
      return false;

   if (perform_rate_request(ctx, str->stream_ep, desc->sample_rate, 1000000) != LIBMARU_SUCCESS)
      return false;

   if (str->feedback_ep && !enqueue_feedback_transfer(ctx, str))
      return false;

   size_t buffer_size = desc->buffer_size;
   if (!buffer_size)
      buffer_size = 1024 * 32;

   // Set fragment size.
   size_t frag_size = desc->fragment_size;
   if (!frag_size)
      frag_size = buffer_size >> 2;

   size_t frame_size = desc->sample_rate * desc->channels * desc->bits / 8;
   frame_size /= 1000;

   str->enqueue_count = frag_size / frame_size + 1;
   if (str->enqueue_count > LIBMARU_MAX_ENQUEUE_COUNT)
      str->enqueue_count = LIBMARU_MAX_ENQUEUE_COUNT;

   str->fifo = maru_fifo_new(buffer_size);
   if (!str->fifo)
      return false;

   size_t read_trigger = frag_size;

   if (maru_fifo_set_read_trigger(str->fifo,
            read_trigger) < 0)
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

   str->bps = desc->sample_rate * desc->channels * desc->bits / 8;
   str->transfer_speed = str->transfer_speed_fraction;
   str->trans_count = 0;

   str->timer.started = false;

   return true;
}

static void deinit_stream_nolock(maru_context *ctx, maru_stream stream)
{
   struct maru_stream_internal *str = &ctx->streams[stream];

   if (str->sync_fd >= 0)
   {
      close(str->sync_fd);
      str->sync_fd = -1;
   }

   if (str->fifo)
   {
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
      &cdesc->interface[ctx->stream_interface].altsetting[ctx->stream_altsetting];

   for (unsigned i = 0; i < desc->bNumEndpoints; i++)
   {
      const struct libusb_endpoint_descriptor *endp = &desc->endpoint[i];

      if (endp->bEndpointAddress < 0x80 &&
            (endp->bmAttributes & USB_ENDPOINT_ISOCHRONOUS))
      {
         if (!add_stream(ctx, endp->bEndpointAddress, endp->bSynchAddress))
            return false;

         if (endp->bmAttributes & USB_ENDPOINT_ADAPTIVE)
            perform_pitch_request(ctx, endp->bEndpointAddress, 100000);
      }
   }

   return true;
}

static bool enumerate_streams(maru_context *ctx,
      const struct maru_stream_desc *desc)
{
   struct libusb_config_descriptor *conf = ctx->conf;

   int ctrl_index = find_interface_class_index(conf,
         USB_CLASS_AUDIO, USB_SUBCLASS_AUDIO_CONTROL, 0, NULL);

   int altsetting = 0;

   int stream_index = find_stream_interface_alt(conf,
         desc, &altsetting);

   if (ctrl_index < 0 || stream_index < 0)
      return false;

   ctx->control_interface = ctrl_index;
   ctx->stream_interface = stream_index;
   ctx->stream_altsetting = altsetting;

#if 0
   fprintf(stderr, "Found interface CTRL: %d, STREAM: %d(%d)\n", ctrl_index, stream_index, altsetting);
#endif

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

   if (libusb_set_interface_alt_setting(ctx->handle, stream_index, altsetting) < 0)
      return false;

   if (!enumerate_endpoints(ctx, conf))
      return false;

   return true;
}

static struct usb_uac_feature_unit_descriptor*
find_volume_feature_unit(const uint8_t *extra, size_t extra_length)
{
   int id = -1;
   struct usb_uac_output_terminal_descriptor *desc = NULL;

   // Find output terminal that maps to a speaker, and see if the last
   // unit in the chain is a feature unit with volume controls.
   for (size_t i = 0; i < extra_length; i += desc->bLength)
   {
      desc = (struct usb_uac_output_terminal_descriptor*)&extra[i];

      if (desc->bLength != sizeof(*desc))
         continue;

      if (desc->bDescriptorSubtype == UAC_OUTPUT_TERMINAL &&
            desc->wTerminalType == UAC_TYPE_SPEAKER)
      {
         id = desc->bSourceID;
         break;
      }
   }

   if (id < 0)
   {
      fprintf(stderr, "Didn't find speaker ...\n");
      return NULL;
   }

   struct usb_uac_feature_unit_descriptor *feature = NULL;

   for (size_t i = 0; i < extra_length; i += feature->bLength)
   {
      feature = (struct usb_uac_feature_unit_descriptor*)&extra[i];

      if (feature->bLength < sizeof(*feature))
         continue;

      if (feature->bDescriptorSubtype == UAC_FEATURE_UNIT &&
            feature->bUnitID == id &&
            feature->bControlSize)
         return feature;
   }

   return NULL;
}

static bool enumerate_controls(maru_context *ctx)
{
   const struct libusb_interface_descriptor *desc = &ctx->conf->interface[ctx->control_interface].altsetting[0];

   struct usb_uac_feature_unit_descriptor *feature =
      find_volume_feature_unit(desc->extra, desc->extra_length);

   if (!feature)
      return false;

   ctx->volume.feature_unit = feature->bUnitID;

   unsigned control_len = feature->bLength - 7;

   // Find the two first channels that have volume control, and assume they map to left/right.
   for (unsigned i = 0; i < control_len && ctx->volume.chans < 2; i += feature->bControlSize)
   {
      if (feature->bmaControls[i] & USB_UAC_VOLUME_SELECTOR)
         ctx->volume.channels[ctx->volume.chans++] = i / feature->bControlSize;
   }

#if 0
   fprintf(stderr, "Enumerated controls:\n");
   fprintf(stderr, "\tUnit: %u\n", ctx->volume.feature_unit);
   fprintf(stderr, "\tChans: %u\n", ctx->volume.chans);
   for (unsigned i = 0; i < ctx->volume.chans; i++)
      fprintf(stderr, "\t\tChan #%u: %u\n", i, ctx->volume.channels[i]);
#endif

   return true;
}

maru_error maru_create_context_from_vid_pid(maru_context **ctx,
      uint16_t vid, uint16_t pid,
      const struct maru_stream_desc *desc)
{
   maru_context *context = calloc(1, sizeof(*context));
   if (!context)
      return LIBMARU_ERROR_MEMORY;

   context->quit_fd = eventfd(0, 0);
   context->epfd = epoll_create(16);
   if (socketpair(AF_UNIX, SOCK_STREAM, 0, context->request_fd) < 0)
      goto error;

   if (fcntl(context->request_fd[0], F_SETFL,
            fcntl(context->request_fd[0], F_GETFL) | O_NONBLOCK) < 0)
      goto error;
   if (fcntl(context->request_fd[1], F_SETFL,
            fcntl(context->request_fd[1], F_GETFL) | O_NONBLOCK) < 0)
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

   if (!enumerate_streams(context, desc))
      goto error;

   if (!enumerate_controls(context))
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

   if (ctx->request_fd[0] >= 0)
      close(ctx->request_fd[0]);
   if (ctx->request_fd[1] >= 0)
      close(ctx->request_fd[1]);

   poll_list_deinit(ctx);

   for (unsigned i = 0; i < ctx->num_streams; i++)
      deinit_stream(ctx, i);
   free(ctx->streams);

   pthread_mutex_destroy(&ctx->lock);

   if (ctx->conf)
      libusb_free_config_descriptor(ctx->conf);

   if (ctx->handle)
   {
      libusb_release_interface(ctx->handle, ctx->control_interface);
      libusb_release_interface(ctx->handle, ctx->stream_interface);
      libusb_attach_kernel_driver(ctx->handle, ctx->control_interface);
      libusb_attach_kernel_driver(ctx->handle, ctx->stream_interface);

      libusb_close(ctx->handle);
   }

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

      if (header->bLength < sizeof(*header))
         continue;

      if (header->bDescriptorType != (USB_CLASS_DESCRIPTOR | USB_INTERFACE_DESCRIPTOR_TYPE) ||
            header->bDescriptorSubtype != USB_FORMAT_DESCRIPTOR_SUBTYPE ||
            header->bFormatType != USB_FORMAT_TYPE_I ||
            ((header->bSamFreqType != USB_FREQ_TYPE_DIRECT) && (header->bSamFreqType != USB_FREQ_TYPE_DISCRETE)))
         continue;

      // FIXME: Parse all sample rates correctly as separate stream descriptions.
      unsigned rate_start = header->bLength - sizeof(*header) - 3;
      // Use last format in list (somewhat hacky, will do for now ...)
      desc->sample_rate =
         (header->tSamFreq[rate_start + 0] <<  0) |
         (header->tSamFreq[rate_start + 1] <<  8) |
         (header->tSamFreq[rate_start + 2] << 16);

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

   const struct libusb_interface_descriptor *iface = &ctx->conf->interface[ctx->stream_interface].altsetting[ctx->stream_altsetting];
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
   if (maru_is_stream_available(ctx, stream) != 0)
      return LIBMARU_ERROR_INVALID;

   // Unblock so we make sure epoll_wait() catches our notification kill.
   poll_list_unblock(ctx->epfd,
         maru_fifo_read_notify_fd(ctx->streams[stream].fifo),
         POLLIN);

   // Wait till thread has acknowledged our close.
   maru_fifo_kill_notification(ctx->streams[stream].fifo);
   uint64_t dummy;
   eventfd_read(ctx->streams[stream].sync_fd, &dummy);

   deinit_stream_nolock(ctx, stream);

   return LIBMARU_SUCCESS;
}

static maru_usec current_time(void)
{
   struct timespec tv;
   clock_gettime(CLOCK_MONOTONIC_RAW, &tv);

   maru_usec time = tv.tv_sec * INT64_C(1000000);
   time += tv.tv_nsec / 1000;
   return time;
}

static void init_timer(struct maru_stream_internal *str)
{
   str->timer.started = true;

   str->timer.start_time = current_time();
   str->timer.write_cnt = 0;
   str->timer.offset = 0;
}

size_t maru_stream_write(maru_context *ctx, maru_stream stream,
      const void *data, size_t size)
{
   if (stream >= ctx->num_streams)
      return 0;

   struct maru_stream_internal *str = &ctx->streams[stream];

   maru_fifo *fifo = str->fifo;
   if (!fifo)
   {
      fprintf(stderr, "Stream has no fifo!\n");
      return 0;
   }

   if (!str->timer.started)
      init_timer(str);

   size_t ret = maru_fifo_blocking_write(fifo, data, size);
   str->timer.write_cnt += ret;
   return ret;
}

int maru_stream_notification_fd(maru_context *ctx,
      maru_stream stream)
{
   if (stream >= ctx->num_streams)
      return LIBMARU_ERROR_INVALID;

   if (!ctx->streams[stream].fifo)
      return LIBMARU_ERROR_INVALID;

   return maru_fifo_write_notify_fd(ctx->streams[stream].fifo);
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
      uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
      void *data, size_t size,
      maru_usec timeout)
{
   struct maru_control_request req = {
      .count        = ctx->request_count++,

      .request_type = request_type,
      .request      = request,
      .value        = value,
      .index        = index,

      .size         = size,

      .reply_fd     = ctx->request_fd[0],
   };

   if (size > sizeof(req.data.data))
      return LIBMARU_ERROR_INVALID;

   memcpy(req.data.data, data, size);

   if (write(ctx->request_fd[1], &req, sizeof(req)) != (ssize_t)sizeof(req))
      return LIBMARU_ERROR_IO;

   if (timeout == 0 && !(request & USB_REQUEST_DIR_MASK))
      return LIBMARU_SUCCESS;

   struct maru_control_request ret_req;

   do
   {
      // Wait for reply from thread.
      struct pollfd fds = {
         .fd = ctx->request_fd[1],
         .events = POLLIN,
      };

      if (poll(&fds, 1, timeout < 0 ? -1 : timeout / 1000) < 0)
      {
         if (errno == EINTR)
            continue;

         return LIBMARU_ERROR_IO;
      }

      if (fds.revents & (POLLHUP | POLLERR | POLLNVAL))
         return LIBMARU_ERROR_IO;

      if (!(fds.revents & POLLIN))
         return LIBMARU_ERROR_TIMEOUT;

      if (read(ctx->request_fd[1], &ret_req, sizeof(ret_req)) != (ssize_t)sizeof(ret_req))
         return LIBMARU_ERROR_IO;

   } while (ret_req.count != req.count);

   memcpy(data, ret_req.data.data, size);
   return ret_req.error;
}

static int perform_pitch_request(maru_context *ctx,
      unsigned ep,
      maru_usec timeout)
{
   return libusb_control_transfer(ctx->handle,
         LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT,
         USB_REQUEST_UAC_SET_CUR,
         UAS_PITCH_CONTROL << 8,
         ep,
         (uint8_t[]) {1}, sizeof(uint8_t), timeout < 0 ? -1 : timeout / 1000);
}

static maru_error perform_rate_request(maru_context *ctx,
      unsigned ep,
      unsigned rate,
      maru_usec timeout)
{
   maru_error err = perform_request(ctx,
         LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT,
         USB_REQUEST_UAC_SET_CUR,
         UAS_FREQ_CONTROL << 8,
         ep,
         (uint8_t[]) { rate >> 0, rate >> 8, rate >> 16 }, 3, timeout);

   if (err != LIBMARU_SUCCESS)
      return err;

   uint8_t new_rate_raw[3];
   err = perform_request(ctx,
         LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT,
         USB_REQUEST_UAC_GET_CUR,
         UAS_FREQ_CONTROL << 8,
         ep,
         new_rate_raw, sizeof(new_rate_raw), timeout);

   if (err != LIBMARU_SUCCESS)
      return err;

   unsigned new_rate =
      (new_rate_raw[0] <<  0) |
      (new_rate_raw[1] <<  8) |
      (new_rate_raw[2] << 16);

   if (new_rate != rate)
   {
      fprintf(stderr, "[libmaru]: Requested %u Hz, got %u Hz.\n",
            rate, new_rate);
      return LIBMARU_ERROR_INVALID;
   }

   return LIBMARU_SUCCESS;
}

static maru_error perform_volume_request(maru_context *ctx,
      maru_volume *vol, uint8_t request, maru_usec timeout)
{
   if (ctx->volume.chans == 0)
      return LIBMARU_ERROR_INVALID;

   uint16_t swapped = libusb_cpu_to_le16(*vol);

   for (unsigned i = 0; i < ctx->volume.chans; i++)
   {
      maru_error err = perform_request(ctx,
            LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE, request,
            (USB_UAC_VOLUME_SELECTOR << 8) | ctx->volume.channels[i],
            (ctx->volume.feature_unit << 8) | ctx->control_interface,
            &swapped, sizeof(swapped), timeout);

      if (err != LIBMARU_SUCCESS)
         return err;
   }

   *vol = libusb_le16_to_cpu(swapped);
   return LIBMARU_SUCCESS;
}

maru_error maru_stream_get_volume(maru_context *ctx,
      maru_stream stream,
      maru_volume *current, maru_volume *min, maru_volume *max,
      maru_usec timeout)
{
   // Only support master channel volume for now.
   if (stream != LIBMARU_STREAM_MASTER)
      return LIBMARU_ERROR_INVALID;

   if (current)
   {
      maru_error err = perform_volume_request(ctx, current,
            USB_REQUEST_UAC_GET_CUR, timeout);

      if (err != LIBMARU_SUCCESS)
         return err;
   }

   if (min)
   {
      maru_error err = perform_volume_request(ctx, min,
            USB_REQUEST_UAC_GET_MIN, timeout);

      if (err != LIBMARU_SUCCESS)
         return err;
   }

   if (max)
   {
      maru_error err = perform_volume_request(ctx, max,
            USB_REQUEST_UAC_GET_MAX, timeout);

      if (err != LIBMARU_SUCCESS)
         return err;
   }

   return LIBMARU_SUCCESS;
}

maru_error maru_stream_set_volume(maru_context *ctx,
      maru_stream stream,
      maru_volume volume,
      maru_usec timeout)
{
   // Only support master channel volume for now.
   if (stream != LIBMARU_STREAM_MASTER)
      return LIBMARU_ERROR_INVALID;

   return perform_volume_request(ctx, &volume, USB_REQUEST_UAC_SET_CUR, timeout);
}

// Estimate current audio latency using high-precision timers.
// This value must be gradually corrected against a low-resolution, but
// "correct" timer value to combat clock skew.
static maru_usec timed_latency(struct maru_stream_internal *stream)
{
   maru_usec since_start = current_time() - stream->timer.start_time;
   maru_usec buffered_time = (stream->timer.write_cnt * 1000000) / stream->bps;

   return buffered_time - since_start + stream->timer.offset;
}

maru_usec maru_stream_current_latency(maru_context *ctx, maru_stream stream)
{
   if (stream >= ctx->num_streams)
      return LIBMARU_ERROR_INVALID;

   struct maru_stream_internal *str = &ctx->streams[stream];

   if (!str->fifo)
      return LIBMARU_ERROR_INVALID;

   if (!str->timer.started)
      return 0;

   // Buffer latency is the maximum possible latency (ignoring USB HW latencies which are unknown).
   maru_usec buffer_latency = (maru_fifo_buffered_size(str->fifo) * INT64_C(1000000)) / str->bps;

   // Chunk latency. buffer_latency - chunk_latency represents the lower bound of latency.
   maru_usec chunk_latency = str->enqueue_count * 1000;

   // Timed latency. May or may not be correct.
   maru_usec timer_latency = timed_latency(str);

   if (timer_latency < 0)
      timer_latency = 0;

   // Timer is completely off, just reset it.
   if (buffer_latency > timer_latency + 2 * chunk_latency ||
         buffer_latency < timer_latency - 2 * chunk_latency)
   {
      init_timer(str);
      str->timer.offset = buffer_latency;
      //fprintf(stderr, "Resetting latency ...\n");
      //fprintf(stderr, "Timer latency = %lld, Buffer latency = %lld\n", (long long)timer_latency, (long long)buffer_latency);
      return buffer_latency;
   }

#define JITTER_USEC_ADJUSTMENT 200
   // Adjust slowly over time.
   //
   // Estimated latency is larger than possible, adjust it down.
   if (timer_latency > buffer_latency)
      str->timer.offset -= JITTER_USEC_ADJUSTMENT;
   // Estimated latency is likely (we can't know for sure) too low, adjust it upwards.
   else if (buffer_latency > timer_latency + chunk_latency)
      str->timer.offset += JITTER_USEC_ADJUSTMENT;

   return timer_latency;
}

const char *maru_error_string(maru_error error)
{
   switch (error)
   {
      case LIBMARU_SUCCESS:
         return "No error";
      case LIBMARU_ERROR_GENERIC:
         return "Generic error";
      case LIBMARU_ERROR_IO:
         return "I/O error";
      case LIBMARU_ERROR_BUSY:
         return "Device is busy";
      case LIBMARU_ERROR_ACCESS:
         return "Permissions error";
      case LIBMARU_ERROR_INVALID:
         return "Invalid argument";
      case LIBMARU_ERROR_MEMORY:
         return "Allocation error";
      case LIBMARU_ERROR_DEAD:
         return "Data structure is dead";
      case LIBMARU_ERROR_TIMEOUT:
         return "Timeout";
      case LIBMARU_ERROR_UNKNOWN:
      default:
         return "Unknown error";
   }
}

