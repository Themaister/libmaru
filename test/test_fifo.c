#include <fifo.h>
#include <pthread.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>
#include <stdio.h>

#define CHUNK_SIZE 2048 * 4
#define BUFFER_SIZE (4096 * 16)

static void *writer_thread(void *data)
{
   maru_fifo *fifo = data;

   char buf[CHUNK_SIZE];
   ssize_t rc;

   maru_fifo_set_write_trigger(fifo, CHUNK_SIZE);

   size_t total = 0;
   while ((rc = read(0, buf, sizeof(buf))) > 0)
   {
      total += rc;
      if (maru_fifo_blocking_write(fifo, buf, rc) < rc)
      {
         fprintf(stderr, "Blocking write failed!\n");
         break;
      }
   }

   struct maru_fifo_locked_region reg;
   maru_fifo_write_lock(fifo, 0, &reg);
   fprintf(stderr, "Write pointer = %p\n", reg.first);
   maru_fifo_write_unlock(fifo, &reg);

   fprintf(stderr, "Writer exiting (last rc = %zi, read %zu bytes from stdin) ...\n", rc, total);
   pthread_exit(NULL);
}

static void *reader_thread(void *data)
{
   maru_fifo *fifo = data;
   char buf[CHUNK_SIZE];

   maru_fifo_set_read_trigger(fifo, CHUNK_SIZE);

   size_t total = 0;
   for (;;)
   {
      size_t ret = maru_fifo_blocking_read(fifo, buf, sizeof(buf));
      if (ret == 0)
      {
         fprintf(stderr, "Reader read %zu bytes before flushing ...\n", total);
         fprintf(stderr, "Flushing ...\n");
         ssize_t rc;
         // Flush out buffer.
         while ((rc = maru_fifo_read(fifo, buf, sizeof(buf))) > 0)
         {
            write(1, buf, rc);
            total += rc;
         }

         fprintf(stderr, "Flush ended with rc = %zi\n", rc);

         struct maru_fifo_locked_region reg;

         maru_fifo_write_lock(fifo, 0, &reg);
         fprintf(stderr, "Read-side write pointer = %p\n", reg.first);
         maru_fifo_write_unlock(fifo, &reg);

         maru_fifo_read_lock(fifo, 0, &reg);
         fprintf(stderr, "Read pointer = %p\n", reg.first);
         maru_fifo_read_unlock(fifo, &reg);

         break;
      }

      write(1, buf, ret);
      total += ret;
   }

   fprintf(stderr, "Reader read %zu bytes ...\n", total);
   fprintf(stderr, "Reader returning ...\n");
   pthread_exit(NULL);
}

int main(void)
{
   maru_fifo *fifo = maru_fifo_new(BUFFER_SIZE);
   assert(fifo);

   pthread_t writer, reader;
   assert(pthread_create(&writer, NULL, writer_thread, fifo) == 0);
   assert(pthread_create(&reader, NULL, reader_thread, fifo) == 0);

   pthread_join(writer, NULL);
   maru_fifo_kill_notification(fifo);
   pthread_join(reader, NULL);

   maru_fifo_free(fifo);
}

