#include "../fifo.h"
#include <pthread.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>

static void *writer_thread(void *data)
{
   maru_fifo *fifo = data;

   maru_fd notify_fd = maru_fifo_write_notify_fd(fifo);
   char buf[32];
   ssize_t rc;
   while ((rc = read(0, buf, sizeof(buf))) > 0)
   {
      struct pollfd fds = { .fd = notify_fd, .events = POLLIN };

      if (poll(&fds, 1, -1) < 0)
         break;

      if (fds.revents & POLLIN)
      {
         if (maru_fifo_write(fifo, buf, rc) < 0)
            break;

         maru_fifo_write_notify_ack(fifo);
      }
      else if (fds.revents & POLLHUP)
         break;
   }

   pthread_exit(NULL);
}

static void *read_thread(void *data)
{
   maru_fifo *fifo = data;
   maru_fd notify_fd = maru_fifo_read_notify_fd(fifo);
   char buf[32];

   for (;;)
   {
      struct pollfd fds = { .fd = notify_fd, .events = POLLIN };

      if (poll(&fds, 1, -1) < 0)
         break;

      if (fds.revents & POLLIN)
      {
         ssize_t ret = maru_fifo_read(fifo, buf, sizeof(buf));
         if (ret < 0)
            break;

         maru_fifo_read_notify_ack(fifo);
         write(1, buf, ret);
      }
      else if (fds.revents & POLLHUP)
      {
         ssize_t ret;
         while ((ret = maru_fifo_read(fifo, buf, sizeof(buf))) > 0)
            write(1, buf, ret);

         break;
      }
   }

   pthread_exit(NULL);
}

int main(void)
{
   maru_fifo *fifo = maru_fifo_new(1024);
   assert(fifo);

   pthread_t writer, reader;
   assert(pthread_create(&writer, NULL, writer_thread, fifo) == 0);
   assert(pthread_create(&reader, NULL, reader_thread, fifo) == 0);

   pthread_join(writer, NULL);
   maru_fifo_kill_notification(fifo);
   pthread_join(reader, NULL);

   maru_fifo_free(fifo);
}

