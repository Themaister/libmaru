#include <libmaru.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

int main(void)
{
   struct maru_audio_device *list;
   unsigned num_devices;

   assert(maru_list_audio_devices(&list, &num_devices) == LIBMARU_SUCCESS);

   fprintf(stderr, "Found %u devices!\n", num_devices);
   for (unsigned i = 0; i < num_devices; i++)
   {
      fprintf(stderr, "Device %u: VID: 0x%04x, PID: 0x%04x\n",
            i, (unsigned)list[i].vendor_id, (unsigned)list[i].product_id);
   }

   if (num_devices > 0)
   {
      maru_context *ctx;
      assert(maru_create_context_from_vid_pid(&ctx, list[0].vendor_id,
               list[0].product_id) == LIBMARU_SUCCESS);

      fprintf(stderr, "Streams: %d\n", maru_get_num_streams(ctx));

      unsigned num_desc;
      struct maru_stream_desc *desc;
      assert(maru_get_stream_desc(ctx, 0, &desc, &num_desc) == LIBMARU_SUCCESS);

      fprintf(stderr, "Format:\n");
      fprintf(stderr, "\tRate: %u\n", desc[0].sample_rate);
      fprintf(stderr, "\tChannels: %u\n", desc[0].channels);
      fprintf(stderr, "\tBits: %u\n", desc[0].bits);

      free(desc);
      maru_destroy_context(ctx);
   }

   free(list);
}

