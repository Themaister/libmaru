#ifndef FIFO_BUFFER_H__
#define FIFO_BUFFER_H__

#include <stddef.h>
#include <stdint.h>
#include "libmaru.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \ingroup buffer
 * Opaque handle to a lockless ring buffer with FIFO semantics. */
typedef struct maru_fifo maru_fifo;

/** \ingroup buffer
 * Type of a native pollable file descriptor. */
typedef int maru_fd;

/** \ingroup buffer
 * \brief Creates a new fifo.
 *
 * \param size Size of buffer.
 * The size of the ring buffer is equal to \c size bytes.
 * Available bytes for writing is also \c size bytes.
 * It is recommended to use a power-of-two sized buffer.
 *
 * \returns Newly allocated fifo, or NULL if failure.
 */
maru_fifo *maru_fifo_new(size_t size);

/** \ingroup buffer
 * \brief Frees a fifo.
 *
 * \param fifo The fifo
 */
void maru_fifo_free(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Get writer side notification handle for fifo.
 *
 * maru_fifo supports a notification handle that can be polled using
 * poll(), select() or similar calls to multiplex different buffers or other file descriptors.
 * The type of the handle may vary with the operating system used. \ref maru_fd.
 *
 * On Unix, the file descriptor can be polled with POLLIN or similar.
 * After an event has been read, it should be acknowledged with \c maru_fifo_write_notify_ack().
 *
 * \param fifo The fifo
 * \returns Platform-specific pollable handle.
 */
maru_fd maru_fifo_write_notify_fd(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Acknowledge a notification.
 *
 * This must be called after a POLLIN has been received by writer.
 * Clears out the event queue.
 *
 * \param fifo The fifo
 */
void maru_fifo_write_notify_ack(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Get reader side notification handle for fifo.
 *
 * This function is the analog for \ref maru_fifo_write_notify_fd.
 * See details there.
 *
 * \param fifo The fifo
 * \returns Platform-specific pollable handle.
 */
maru_fd maru_fifo_read_notify_fd(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Acknowledge a notification
 *
 * This must be called after a POLLIN has been received by reader.
 * Clears out the event queue.
 *
 * \param fifo The fifo
 */
void maru_fifo_read_notify_ack(maru_fifo *fifo);

size_t maru_fifo_read_avail(maru_fifo *fifo);
size_t maru_fifo_write_avail(maru_fifo *fifo);

maru_error maru_fifo_write_lock(maru_fifo *fifo,
      size_t size,
      void **data_first, size_t *data_first_size,
      void **data_second, size_t *data_second_size);

maru_error maru_fifo_write_unlock(maru_fifo *fifo,
      void *data_first, size_t data_first_size,
      void *data_second, size_t data_second_size);

maru_error maru_fifo_read_lock(maru_fifo *fifo,
      size_t size,
      const void **data_first, size_t *data_first_size,
      const void **data_second, size_t *data_second_size);

maru_error maru_fifo_read_unlock(maru_fifo *fifo,
      const void *data_first, size_t data_first_size,
      const void *data_second, size_t data_second_size);

size_t maru_fifo_write(maru_fifo *fifo, const void *data, size_t size);
size_t maru_fifo_read(maru_fifo *fifo, void *data, size_t size);

#ifdef __cplusplus
}
#endif
#endif

