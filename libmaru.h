#ifndef LIBMARU_H__
#define LIBMARU_H__

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#include <stdint.h>
#include <poll.h>
#include <limits.h>

/** \ingroup lib
 * A structure describing a USB audio device connected to the system.
 */
struct maru_audio_device
{
   /** Vendor ID of connected device. */
   uint16_t vendor_id;

   /** Product ID of connected device. */
   uint16_t product_id;

   /** Tells if the audio interfaces of the device are claimed by the kernel.
    * To use this device, libmaru will attempt to take control of the device.
    * If a device is not claimed by the kernel,
    * some other process might have taken control of it. */
   bool claimed_by_kernel;
};

/** \ingroup lib
 * A structure defining pollable file descriptors. */
struct maru_pollfd
{
   /** Unix file descriptor. Must only be used for polling purposes. */
   int fd; 

   /** \c poll() compatible events field. */
   short events;
};

/** \ingroup lib
 * Opaque type representing a libmaru context.
 * A single context represents the state of a connected device,
 * and its associated streams.
 */
typedef struct maru_context maru_context;

/** \ingroup lib
 * Opaque type representing an active audio stream.
 * If supported by the device, multiple audio streams can be multiplexed in the same context.
 */
typedef unsigned maru_stream;

/** \ingroup lib
 * General type for errors emitted in libmaru.
 */
typedef enum
{
   LIBMARU_SUCCESS         =  0, /**< Success (no error) */
   LIBMARU_ERROR_GENERIC   = -1, /**< Generic error. Used where a more specific error does not apply. */
   LIBMARU_ERROR_IO        = -2, /**< I/O error. Indicates issue with hardware interface */
   LIBMARU_ERROR_BUSY      = -3, /**< libmaru is busy, and might be able to deal with the request later */
   LIBMARU_ERROR_ACCESS    = -4, /**< Lack of privileges.
                                   Often occurs if USB subsystem needs root privileges, and caller is a user. */
   LIBMARU_ERROR_INVALID   = -5, /**< Invalid argument */
   LIBMARU_ERROR_UNKNOWN   = INT_MIN /**< Unknown error (Also used to enforce int size of enum) */
} maru_error;

/** \ingroup lib
 * \brief Translate \ref maru_error to human readable string.
 */
const char *maru_error_string(maru_error error);

/** \ingroup lib
 * \brief List all connected USB audio devices in the system.
 *
 * Get a list of all connected USB devices that advertise themselves as
 * complying to the USB audio class.
 *
 * \param list Pointer to a list that will be allocated.
 * If this function returns successfully and num_devices is larger than 0,
 * caller must call \c free() on the list when not needed anymore.
 *
 * \param num_devices Receives number of devices found.
 *
 * \returns Error code \ref maru_error
 */
maru_error maru_list_audio_devices(struct maru_audio_device **list, unsigned *num_devices);

/** \ingroup lib
 * \brief Create new context from vendor and product IDs.
 *
 * Creates a new context for a device.
 * Will attempt to claim the interfaces necessary from kernel.
 * Opening a device might require root privileges, depending on the system.
 *
 * \param ctx Pointer to a context that is to be initialized.
 * \param vid Vendor ID
 * \param pid Product ID
 *
 * \returns Error code \ref maru_error
 */
maru_error maru_create_context_from_vid_pid(maru_context **ctx, uint16_t vid, uint16_t pid);

/** \ingroup lib
 * \brief Destroy previously allocated context.
 *
 * \param ctx libmaru context.
 */
void maru_destroy_context(maru_context *ctx);

/** \ingroup lib
 * \brief Returns number of hardware streams in total.
 *
 * \param ctx libmaru context
 * \returns Available hardware streams.
 * If negative, the value represents an error code \ref maru_error.
 */
int maru_get_available_streams(maru_context *ctx);

/** \ingroup lib
 * \brief Checks if a stream is currently being used.
 *
 * \param ctx libmaru context
 * \param stream Stream index. Possible indices are in the range of
 * [0, \c maru_get_available_streams() - 1] inclusive.
 *
 * \returns 1 if stream can be used, 0 if it is already being used, negative if error \ref maru_error occured.
 */
int maru_is_stream_available(maru_context *ctx, maru_stream stream);

/** \ingroup lib
 * \brief Finds first available stream.
 *
 * \param ctx libmaru context
 * \returns Available stream is returned. If error, error code \ref maru_error is returned.
 */
int maru_find_available_stream(maru_context *ctx);

/** \ingroup stream
 * A struct describing audio stream parameters for plain PCM streams.
 *
 * Endianness of PCM data is omitted,
 * and is always assumed to be little-endian
 * as it's the native endianness per USB specification.
 */
struct maru_stream_desc
{
   /** Sample rate of PCM audio.
    * If 0, \ref sample_rate_min and \ref sample_rate_max are set by \c maru_get_stream_desc().
    * Application must fill in sample_rate in that case. */
   unsigned sample_rate;
   /** Number of PCM channels. */
   unsigned channels;
   /** Number of bits per audio sample. */
   unsigned bits;

   /** Desired buffer size in bytes. This value might not be honored exactly.
    * It is not set by \c maru_get_stream_desc().
    * If a buffer size of 0 is passed to \c maru_open_stream(),
    * it will attempt to find some appropriate buffer size. */
   size_t buffer_size;

   /** Might be set by \c maru_get_stream_desc() if the endpoint supports continous sample rates.
    * \ref sample_rate will still be set to an appropriate value even if these fields are set. */
   unsigned sample_rate_min;

   /** Might be set by \c maru_get_stream_desc() if the endpoint supports continous sample rates.
    * \ref sample_rate will still be set to an appropriate value even if these fields are set. */
   unsigned sample_rate_max;
};

/** \ingroup stream
 * \brief Obtains all supported \ref maru_stream_desc for a given stream.
 *
 * \param ctx libmaru context
 * \param stream Stream index. Must not be open.
 * \param desc Pointer to list of stream descriptor.
 * If num_desc is larger than 0, caller must call \c free() on returned desc.
 * \param num_desc Number of descriptors found.
 * \returns Error code \ref maru_error
 */
int maru_get_stream_desc(maru_context *ctx, maru_stream stream,
      struct maru_stream_desc **desc, unsigned *num_desc);

/** \ingroup stream
 * \brief Opens an available stream, and readies it for writing.
 *
 * \param ctx libmaru context
 * \param stream Stream index to use. Must be an available stream.
 * \ref maru_is_stream_available \ref maru_find_available_stream
 * \param desc
 *
 * \returns Error code \ref maru_error
 */
int maru_open_stream(maru_context *ctx, maru_stream stream, const struct maru_stream_desc *desc);

/** \ingroup stream
 * \brief Closes an opened stream.
 *
 * \param ctx libmaru context
 * \param stream Stream index.
 *
 * \returns Error code \ref maru_error
 */
int maru_close_stream(maru_context *ctx, maru_stream stream);

#ifdef __cplusplus
}
#endif

#endif

