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

#ifndef FIFO_BUFFER_H__
#define FIFO_BUFFER_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "libmaru.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \ingroup buffer
 * Opaque handle to a lockless ring buffer with FIFO semantics. */
typedef struct maru_fifo maru_fifo;

/** \ingroup buffer
 * \brief Creates a new fifo.
 *
 * \param size Size of buffer.
 * The size of the ring buffer is equal to \c size bytes.
 * Available bytes for writing is \c size bytes - 1.
 * The size of the fifo will be rounded up to
 * the nearest power-of-two size for efficiency.
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
 *
 * On Unix, the file descriptor can be polled with POLLIN or similar.
 * After an event has been read, and the user has performed the needed operations on the buffer,
 * the buffer should be signaled with \c maru_fifo_write_notify_ack().
 *
 * Even after a POLLIN is received, it is possible that available write size is smaller than desired.
 * In this case, maru_fifo_write_notify_ack() must still be called.
 *
 * \param fifo The fifo
 * \returns Platform-specific pollable handle.
 */
int maru_fifo_write_notify_fd(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Acknowledge a notification.
 *
 * This must be called after a POLLIN has been received by writer.
 * Clears out the event queue.
 * If notifications have been killed with maru_fifo_kill_notification() earlier,
 * return value will notify of a kill.
 * If fifo is killed, event queue will not be cleared out when calling this function.
 *
 * \param fifo The fifo
 * \returns Error code \ref maru_error.
 * If error is returned, the fifo is dead, and cannot be used anymore.
 */
maru_error maru_fifo_write_notify_ack(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Get total amount of data currently in buffer.
 *
 * Functionally equivalent to buffer_size - writable_size - 1.
 * Useful in audio cases to calculate latency.
 */
size_t maru_fifo_buffered_size(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Get reader side notification handle for fifo.
 *
 * This function is the analog for \ref maru_fifo_write_notify_fd.
 * See details there.
 *
 * \param fifo The fifo
 * \returns Platform-specific pollable handle.
 */
int maru_fifo_read_notify_fd(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Set the least amount of bytes that needs to be writable for notification to occur.
 *
 * This function is used to give better control over situations where you need a certain amount
 * of data to be available for POLLIN to occur on the notification descriptor.
 * 
 * Do note that using a trigger on both writes and reads could potentially lead to a deadlock if the triggers overlap.
 * This is typically the case if write_edge + read_edge >= buffer_size.
 * This function will attempt to detect this case, but do not rely on it.
 * 
 * \param fifo The fifo
 * \param size The number of bytes that must be available at a minimum for POLLIN to occur.
 * Setting 0 bytes is treated as a value of 1 (default).
 * \returns Error code \ref maru_error.
 */
maru_error maru_fifo_set_write_trigger(maru_fifo *fifo, size_t size);

/** \ingroup buffer
 * \brief Set the least amount of bytes that needs to be readable for notification to occur.
 *
 * This function is used to give better control over situations where you need a certain amount
 * of data to be available for POLLIN to occur on the notification descriptor.
 * 
 * Do note that using a trigger on both writes and reads could potentially lead to a deadlock if the triggers overlap.
 * This is typically the case if write_edge + read_edge >= buffer_size.
 * This function will attempt to detect this case, but do not rely on it.
 * 
 * \param fifo The fifo
 * \param size The number of bytes that must be available at a minimum for POLLIN to occur.
 * Setting 0 bytes is treated as a value of 1 (default).
 * \returns Error code \ref maru_error.
 */
maru_error maru_fifo_set_read_trigger(maru_fifo *fifo, size_t size);

/** \ingroup buffer
 * \brief Acknowledge a notification
 *
 * This must be called after a POLLIN has been received by reader.
 * Clears out the event queue.
 * If notifications have been killed with maru_fifo_kill_notification() earlier,
 * return value will notify of a kill.
 * If fifo is killed, event queue will not be cleared out when calling this function.
 *
 * \param fifo The fifo
 * \returns Error code \ref maru_error.
 * If error is returned, the fifo is dead, and cannot be used anymore.
 */
maru_error maru_fifo_read_notify_ack(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Kill notification handles.
 * Polling notification handles after this will always give POLLIN,
 * allowing for clean tear-down in a threaded environment.
 * To detect that notification was killed,
 * maru_fifo_read_notify_ack() and maru_fifo_write_notify_ack() will return error.
 * 
 * After killing notification it is still possible to perform writes and reads (to flush out the last data),
 * but the blocking interfaces will return errors (even if there is data available).
 */
void maru_fifo_kill_notification(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Returns number of readable bytes in buffer.
 *
 * Returns number of readable bytes that have yet to be claimed by earlier calls to \c maru_fifo_read_lock.
 *
 * \param fifo The fifo
 * \returns Readable bytes in buffer.
 */
size_t maru_fifo_read_avail(maru_fifo *fifo);

/** \ingroup buffer
 * \brief Returns number of writable bytes in buffer.
 *
 * Returns number of writeable bytes that have yet to be claimed by earlier calls to \c maru_fifo_write_lock.
 *
 * \param fifo The fifo
 * \returns Writable bytes in buffer.
 */
size_t maru_fifo_write_avail(maru_fifo *fifo);

/** Structure that represents the state of a locked region of the ring buffer.
 * Due to the ring-y nature of the buffer, a locked region might be split into two. */
struct maru_fifo_locked_region
{
   /** Pointer to first region of the locked region. */
   void *first;
   /** Size of the first region. */
   size_t first_size;

   /** Optionally a pointer to second locked region if first wraps around.
    * Should first point to a fully contigous region, second is set to NULL. */
   void *second;

   /** Size of second locked region.
    * Is set to 0 if data_second is set to NULL. */
   size_t second_size;
};

/** \ingroup buffer
 * \brief Lock out a region of the fifo for writing.
 *
 * Locking a buffer still allows other operations to continue on this buffer,
 * however, locking out a part of the buffer reduces the writable size.
 * It is also possible to lock a section of the buffer if it's already locked,
 * however, great care must be taken to ensure that the order of unlocks is the same as the order of locks.
 *
 * Due to the ring-y nature of the buffer,
 * locking the buffer might give you two different regions as it can wrap around at the end.
 *
 * \param fifo The fifo
 * \param size The size to lock. If size is larger than \c maru_fifo_write_avail(), the result is undefined.
 * \param region Locked region to be set. If successful, a comparably equal struct must be passed to \c maru_fifo_write_unlock
 * at a later time.
 *
 * \returns Error code \ref maru_error
 */
maru_error maru_fifo_write_lock(maru_fifo *fifo,
      size_t size, struct maru_fifo_locked_region *region);

/** \ingroup buffer
 * \brief Unlock a region of the fifo that was previously locked.
 *
 * Unlocking a buffer allows the reading side to read more data.
 * It also notifies the reading side via notification descriptor.
 * After unlocking, pointers region->first and region->second are considered invalid.
 *
 * \param fifo The fifo
 * \param region The region that was earlier obtained from \c maru_fifo_write_lock
 *
 * \returns Error code \ref maru_error
 */
maru_error maru_fifo_write_unlock(maru_fifo *fifo, const struct maru_fifo_locked_region *region);

/** \ingroup buffer
 * \brief Lock out a region of the fifo for reading.
 *
 * Locking a buffer still allows other operations to continue on this buffer,
 * however, locking out a part of the buffer reduces the readable size.
 * It is also possible to lock a section of the buffer if it's already locked,
 * however, great care must be taken to ensure that the order of unlocks is the same as the order of locks.
 *
 * Due to the ring-y nature of the buffer,
 * locking the buffer might give you two different regions as it can wrap around at the end.
 *
 * \param fifo The fifo
 * \param size The size to lock. If size is larger than \c maru_fifo_read_avail(), the result is undefined.
 * \param region Locked region to be set. If successful, a comparably equal struct must be passed to \c maru_fifo_read_unlock
 * at a later time.
 *
 * \returns Error code \ref maru_error
 */
maru_error maru_fifo_read_lock(maru_fifo *fifo,
      size_t size, struct maru_fifo_locked_region *region);

/** \ingroup buffer
 * \brief Unlock a region of the fifo that was previously locked.
 *
 * Unlocking a buffer allows the writing side to write more data.
 * It also notifies the writing side via notification descriptor.
 * After unlocking, pointers region->first and region->second are considered invalid.
 *
 * \param fifo The fifo
 * \param region The region that was earlier obtained from \c maru_fifo_read_lock
 *
 * \returns Error code \ref maru_error
 */
maru_error maru_fifo_read_unlock(maru_fifo *fifo, const struct maru_fifo_locked_region *region);

/** \ingroup buffer
 * \brief Write data to fifo in a simple manner.
 *
 * Simple interface for writing to fifo.
 * Allows size to be larger than writable size, but not all data might be written in this case.
 * Cannot call this function if a writer lock is being held.
 *
 * \param fifo The fifo
 * \param data Data to write
 * \param size Size of data
 *
 * \returns Number of bytes written. Returns -1 on error.
 */
ssize_t maru_fifo_write(maru_fifo *fifo, const void *data, size_t size);

/** \ingroup buffer
 * \brief Read data from fifo in a simple manner.
 *
 * Simple interface for reading from fifo.
 * Allows size to be larger than readable size, but not all data might be read in this case.
 * Cannot call this function if a reader lock is being held.
 *
 * \param fifo The fifo
 * \param data Buffer to read into
 * \param size Size to read
 *
 * \returns Number of bytes read. Returns -1 on error.
 */
ssize_t maru_fifo_read(maru_fifo *fifo, void *data, size_t size);

/** \ingroup buffer
 * \brief Write all data to fifo in a blocking fashion.
 *
 * Very simple interface for writing to the fifo.
 * Will attempt in a blocking fashion to write all data to the buffer.
 * Cannot call this function if a write lock is being held.
 *
 * If more control over blocking is desired, maru_fifo_write() and maru_fifo_read() are more appropriate.
 *
 * \param fifo The fifo
 * \param data Buffer to read into
 * \param size Size to read
 *
 * \returns Number of bytes written.
 * On error, return value will be smaller than size, and the return value represents the
 * number of bytes written before error occured.
 */
size_t maru_fifo_blocking_write(maru_fifo *fifo, const void *data, size_t size);

/** \ingroup buffer
 * \brief Write all data to fifo in a blocking fashion.
 *
 * Very simple interface for writing to the fifo.
 * Will attempt in a blocking fashion to write all data to the buffer.
 * Cannot call this function if a read lock is being held.
 *
 * If more control over blocking is desired, maru_fifo_write() and maru_fifo_read() are more appropriate.
 *
 * \param fifo The fifo
 * \param data Buffer to read into
 * \param size Size to read
 *
 * \returns Number of bytes written.
 * On error or if notification is shut down, return value will be smaller than size, and the return value represents the
 * number of bytes written before this condition occured.
 * If there is still data to be read from the buffer after a notification kill,
 * it can be read with maru_fifo_read().
 */
size_t maru_fifo_blocking_read(maru_fifo *fifo, void *data, size_t size);

#ifdef __cplusplus
}
#endif
#endif

