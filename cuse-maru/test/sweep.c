/*  cuse-maru - CUSE implementation of Open Sound System using libmaru.
 *  Copyright (C) 2012 - Hans-Kristian Arntzen
 *  Copyright (C) 2012 - Agnes Heyer
 *
 *  cuse-maru is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  cuse-maru is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with cuse-maru.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

int main(int argc, char *argv[])
{
   if (argc != 5)
   {
      fprintf(stderr, "Usage: %s <OSS device> <start_rate> <stop_rate> <sample_rate>\n", argv[0]);
      return 1;
   }

   int fd = open(argv[1], O_WRONLY);
   if (fd < 0)
      return 1;

   double start_rate = strtod(argv[2], NULL);
   double stop_rate = strtod(argv[3], NULL);
   if (stop_rate <= start_rate)
      return 1;

   int sample_rate = strtol(argv[4], NULL, 0);

   if (ioctl(fd, SNDCTL_DSP_SPEED, &sample_rate) < 0)
      return 1;
   if (ioctl(fd, SNDCTL_DSP_CHANNELS, &(int){2}) < 0)
      return 1;
   if (ioctl(fd, SNDCTL_DSP_SETFMT, &(int){AFMT_S16_NE}) < 0)
      return 1;

   double start_omega = 2.0 * M_PI * start_rate / sample_rate;
   double stop_omega = 2.0 * M_PI * stop_rate / sample_rate;

   int16_t buffer[2 * 1024];
   unsigned buffer_ptr = 0;

   const unsigned range = sample_rate * (stop_rate - start_rate) / 1000;
   const double freq_coeff = (stop_omega - start_omega) / range;

   for (unsigned i = 0; i < range; i++)
   {
      int16_t sample = 0x7ff0 * cos(start_omega * i + freq_coeff * 0.5 * i * i); // w = start_omega + freq_coeff * i

      buffer[buffer_ptr++] = sample;
      buffer[buffer_ptr++] = sample;

      if (buffer_ptr >= sizeof(buffer) / sizeof(int16_t))
      {
         write(fd, buffer, sizeof(buffer));
         buffer_ptr = 0;
      }
   }

   close(fd);
}
