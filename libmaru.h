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
#include <stddef.h>

/** \ingroup lib
 * A structure describing a USB audio device connected to the system.
 */
struct maru_audio_device
{
   /** Vendor ID of connected device. */
   uint16_t vendor_id;

   /** Product ID of connected device. */
   uint16_t product_id;
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
 * Type representing time in microseconds (10^-6 sec).
 */
typedef int64_t maru_usec;

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
   LIBMARU_ERROR_MEMORY    = -6, /**< Memory allocation error */
   LIBMARU_ERROR_DEAD      = -7, /**< Data structure is dead */
   LIBMARU_ERROR_TIMEOUT   = -8, /**< Request timed out */
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
int maru_get_num_streams(maru_context *ctx);

/** \ingroup lib
 * \brief Checks if a stream is currently being used.
 *
 * \param ctx libmaru context
 * \param stream Stream index. Possible indices are in the range of
 * [0, \c maru_get_num_streams() - 1] inclusive.
 *
 * \returns 1 if stream can be used, 0 if it is already being used, negative if error \ref maru_error occured.
 */
int maru_is_stream_available(maru_context *ctx, maru_stream stream);

/** \ingroup lib
 * \brief Finds first available stream.
 *
 * This function will look through available streams
 * and attempt to find the first vacant stream.
 * Even if this function finds a stream,
 * a different thread could potentially claim
 * the stream in question before the thread
 * calling maru_find_available_stream()
 * can actually claim it. If this is a likely scenario,
 * maru_find_available_stream() should be called again,
 * until a stream is successfully created or fails.
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
    * It is not set by maru_get_stream_desc().
    * If a buffer size of 0 is passed to maru_open_stream(),
    * it will attempt to find some appropriate buffer size. */
   size_t buffer_size;

   /** Fragment size of writes, the minimum data that must be available
    * for writing for the fifo to notify blocking write functions.
    *
    * It is not set by maru_get_stream_desc().
    *
    * A higher value here is good for CPU usage
    * as data will be moved in larger chunks, which means less polling.
    * A higher fragment size will lead to more latency,
    * but is recommended for non-latency critical applications like audio players and video players where you want to reduce CPU usage of audio as much as possible.
    *
    * A common fragment size is 1/4th or 1/8th the size of the buffer.
    * If you plan to write a constant amount of data every blocking write,
    * that size should be used as fragment_size to optimize a blocking write to a single poll.
    *
    * If a fragment size of 0 is passed to maru_open_stream(),
    * it will attempt to find some appropriate value from the buffer size.
    */
   size_t fragment_size;

   /** Might be set by \c maru_get_stream_desc() if the endpoint supports continous sample rates.
    * \ref sample_rate will not be set to an appropriate value if these fields are set. */
   unsigned sample_rate_min;

   /** Might be set by \c maru_get_stream_desc() if the endpoint supports continous sample rates.
    * \ref sample_rate will not be set to an appropriate value if these fields are set. */
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
maru_error maru_get_stream_desc(maru_context *ctx, maru_stream stream,
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
maru_error maru_stream_open(maru_context *ctx, maru_stream stream, const struct maru_stream_desc *desc);

/** \ingroup stream
 * \brief Closes an opened stream.
 *
 * \param ctx libmaru context
 * \param stream Stream index
 *
 * \returns Error code \ref maru_error
 */
maru_error maru_stream_close(maru_context *ctx, maru_stream stream);

/** \ingroup stream
 * \brief Callback type that can be used to signal the caller when something of interest to the caller has occured.
 *
 * The notification callbacks are usually called from a different thread, and normal thread safety considerations apply. Calling libmaru functions from within this callback is unspecified, and is likely to deadlock.
 */
typedef void (*maru_notification_cb)(void *userdata);

/** \ingroup stream
 * \brief Write all data in a blocking fashion.
 *
 * Writes all data in a blocking fashion.
 * If non-blocking operation is desired, a process should check maru_stream_write_avail().
 *
 * \param ctx libmaru context
 * \param stream Stream index
 * \param data Data to write
 * \param size Size to write
 *
 * \returns Bytes written. If returned amount is lower than size, an error occured.
 */
size_t maru_stream_write(maru_context *ctx, maru_stream stream, 
      const void *data, size_t size);

/** \ingroup stream
 * \brief Checks how much data can be written without blocking.
 *
 * If an attempt is made to write a larger amount than maru_stream_write_avail(),
 * it may block for an indefinite time. Unless required by your application, it is recommended to
 * use the blocking interface.
 *
 * \param ctx libmaru context
 * \param stream Stream index
 * \returns Bytes available for writing without blocking.
 */
size_t maru_stream_write_avail(maru_context *ctx, maru_stream stream);

/** \ingroup stream
 * \brief Set notification callback to be called after data has been processed and is ready for more data.
 *
 * \param ctx libmaru context
 * \param stream Stream index
 * \param callback Callback to call. If NULL, this notification will not be issued.
 * \param userdata Application defined data. Data here will be passed to callback \ref maru_notification_cb.
 */
void maru_stream_set_write_notification(maru_context *ctx, maru_stream stream,
      maru_notification_cb callback, void *userdata);

/** \ingroup stream
 * \brief Set notification callback to be called when an unforeseen error occurs.
 *
 * \param ctx libmaru context
 * \param stream Stream index
 * \param callback Callback to call. If NULL, this notification will not be issued.
 * \param userdata Application defined data. Data here will be passed to callback \ref maru_notification_cb.
 */
void maru_stream_set_error_notification(maru_context *ctx, maru_stream stream,
      maru_notification_cb callback, void *userdata);

/** \ingroup stream
 * \brief Returns current audio latency in microseconds.
 *
 * \param ctx libmaru context
 * \param stream Stream index
 * \returns Latency in microseconds, or a negative number if error \ref maru_error.
 */
maru_usec maru_stream_current_latency(maru_context *ctx, maru_stream stream);

/**
 * \brief Typedef for a volume value. It is encoded in dB fixed point
 * where the actual value is (val) / 256.0. The number is signed and matches
 * the USB audio specification. */
typedef int16_t maru_volume_t;

/**
 * \brief Pseudo-stream that represents the final output stream (after mixing).
 * Only to be used with the volume control. */
#define LIBMARU_STREAM_MASTER ((maru_stream)-1)

/**
 * \brief Pseudo-volume representing mute */
#define LIBMARU_VOLUME_MUTE ((maru_volume_t)-0x8000)

/** \ingroup stream
 * \brief Gets available volume range for stream.
 *
 * This operation is blocking, as requests have to be asynchronously
 * issues to the USB subsystem.
 * Considerations must be made if this function is to be used in
 * a GUI or similar where blocking operations are bad.
 * A control request like this can usually be completed in the order of
 * 5ms.
 *
 * A volume request can be performed concurrently with other stream calls, however, only one thread can perform volume handling at a time.
 *
 * \param ctx libmaru context
 * \param stream Stream to query.
 * If stream is set to the pseudo-stream LIBMARU_STREAM_MASTER,
 * volume control for the mixer output (master) is queried.
 *
 * \param current Outputs current volume. Can be NULL if this information is not required.
 * \param min Outputs minimum volume. Can be NULL if this information is not required.
 * \param max Outputs maximum volume. Can be NULL if this information is not required.
 * \param timeout Timeout for volume control in microseconds \ref maru_usec.
 * A timeout of 0 will in practice never work. A negative timeout will block until completion
 * or error has occured.
 * This timeout is per request. If current, min and max are all desired, the timeout will be applied
 * per request, effectively tripling the timeout.
 * \returns Error code \ref maru_error
 */
maru_error maru_stream_get_volume(maru_context *ctx,
      maru_stream stream,
      maru_volume_t *current, maru_volume_t *min, maru_volume_t *max,
      maru_usec timeout);

/** \ingroup stream
 * \brief Sets volume for a stream.
 *
 * This function behaves similar to maru_stream_get_volume(). See its reference for considerations on use.
 *
 * \param ctx libmaru context
 * \param stream Stream to set volume to.
 * If stream is set to the pseudo-stream LIBMARU_STREAM_MASTER, volume for mixer output (master) is set.
 * \param volume Volume to set. See \ref maru_volume_t on how the volume should be encoded.
 * \param timeout Timeout of request. See maru_stream_get_volume() for more considerations.
 * Volume can also be set to the LIBMARU_VOLUME_MUTE constant to mute.
 *
 * \returns Error code \ref maru_error.
 */
maru_error maru_stream_set_volume(maru_context *ctx,
      maru_stream stream,
      maru_volume_t volume,
      maru_usec timeout);

#ifdef __cplusplus
}
#endif

#endif

