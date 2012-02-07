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

   free(list);
}
