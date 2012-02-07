#include "libmaru.h"
#include <libusb/libusb.h>
#include <stdlib.h>

#define USB_CLASS_AUDIO 1
#define USB_SUBCLASS_AUDIO_STREAMING 2

static bool interface_is_audio_class(const struct libusb_interface_descriptor *iface)
{
   return iface->bInterfaceClass == USB_CLASS_AUDIO &&
         iface->bInterfaceSubClass == USB_SUBCLASS_AUDIO_STREAMING;
}

static bool device_is_audio_class(libusb_context *ctx, libusb_device *dev)
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

   for (unsigned i = 0; i < conf->bNumInterfaces; i++)
   {
      for (int j = 0; j < conf->interface[i].num_altsetting; j++)
      {
         if (interface_is_audio_class(&conf->interface[i].altsetting[j]))
         {
            is_audio = true;
            goto end;
         }
      }
   }

end:
   libusb_free_config_descriptor(conf);
   return is_audio;
}

static bool fill_vid_pid(libusb_device *dev,
      struct maru_audio_device *dev)
{
   struct libusb_device_descriptor desc;
   if (libusb_get_device_descriptor(dev, &desc) < 0)
      return false;

   dev->vendor_id = desc.idVendor;
   dev->product_id = desc.idProduct;

   return true;
}

static bool enumerate_audio_devices(libusb_context *ctx,
      libusb_device **list, unsigned devices,
      struct maru_audio_device **audio_list, unsigned *num_devices)
{
   size_t audio_devices = 0;
   size_t audio_devices_size = 0;
   struct maru_audio_devices *audio_dev = NULL;

   for (ssize_t i = 0; i < devices; i++)
   {
      if (!device_is_audio_class(list[i]))
         continue;

      if (audio_devices >= audio_devices_size)
      {
         audio_devices_size = 2 * audio_devices_size + 1;

         struct maru_audio_devices *new_dev = realloc(audio_dev,
               audio_devices_size * sizeof(struct maru_audio_device));

         if (!new_dev)
         {
            free(audio_dev);
            return false;
         }

         audio_dev = new_dev;
      }

      struct maru_audio_device dev;
      if (!fill_vid_pid(list[i], &audio_dev[audio_devices]))
         continue;

      audio_devices++;
   }

   *list = audio_dev;
   *num_devices = audio_devices;
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
   return LIBUSB_ERROR_MEMORY;
}
