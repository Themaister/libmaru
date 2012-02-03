#include "../fifo.h"
#include <pthread.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>
#include <stdio.h>

static void *writer_thread(void *data)
{
   maru_fifo *fifo = data;

   char buf[4];
   ssize_t rc;

   size_t total = 0;
   while ((rc = read(0, buf, sizeof(buf))) > 0)
   {
      total += rc;
      if (maru_fifo_blocking_write(fifo, buf, rc) < rc)
         break;
   }

   fprintf(stderr, "Writer exiting (last rc = %zi, read %zu bytes from stdin ...\n", rc, total);
   pthread_exit(NULL);
}

static void *reader_thread(void *data)
{
   maru_fifo *fifo = data;
   char buf[4];

   for (;;)
   {
      size_t ret = maru_fifo_blocking_read(fifo, buf, sizeof(buf));
      if (ret == 0)
         break;

      write(1, buf, ret);
   }

   fprintf(stderr, "Reader returning ...\n");
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

