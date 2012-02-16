#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#ifndef SNDCTL_DSP_GETPLAYVOL
#define SNDCTL_DSP_GETPLAYVOL _SIOR('P', 24, int)
#endif

#ifndef SNDCTL_DSP_SETPLAYVOL
#define SNDCTL_DSP_SETPLAYVOL _SIOWR('P', 24, int)
#endif

static int get_volume(int fd)
{
   int val;
   if (ioctl(fd, SNDCTL_DSP_GETPLAYVOL, &val) < 0)
   {
      perror("ioctl");
      return 1;
   }

   printf("%d\n", val & 0xff);
   return 0;
}

static int set_volume(int fd, int argc, char *argv[])
{
   if (argc < 4)
   {
      fprintf(stderr, "Needs argument for volume to set ...\n");
      return 1;
   }

   int val = strtol(argv[3], NULL, 0);

   if (ioctl(fd, SNDCTL_DSP_SETPLAYVOL, &val) < 0)
   {
      perror("ioctl");
      return 1;
   }

   return 0;
}

static int delta_volume(int fd, int argc, char *argv[])
{
   if (argc < 4)
   {
      fprintf(stderr, "Needs argument for delta ...\n");
      return 1;
   }

   int val = strtol(argv[3], NULL, 0);

   int current;
   if (ioctl(fd, SNDCTL_DSP_GETPLAYVOL, &current) < 0)
   {
      perror("ioctl");
      return 1;
   }

   current &= 0xff;
   current += val;

   if (current < 0)
      current = 0;
   else if (current > 100)
      current = 100;

   current |= (current << 8);

   if (ioctl(fd, SNDCTL_DSP_SETPLAYVOL, &current) < 0)
   {
      perror("ioctl");
      return 1;
   }

   return 0;
}

int main(int argc, char *argv[])
{
   if (argc < 3)
   {
      fprintf(stderr, "Usage: <dsp> <get|set|delta> <vol (0-100)>\n");
      return 1;
   }

   int fd = open(argv[1], O_WRONLY);
   if (fd < 0)
   {
      perror("open");
      return 1;
   }

   if (strcmp(argv[2], "get") == 0)
      return get_volume(fd);
   else if (strcmp(argv[2], "set") == 0)
      return set_volume(fd, argc, argv);
   else if (strcmp(argv[2], "delta") == 0)
      return delta_volume(fd, argc, argv);
   else
   {
      fprintf(stderr, "Invalid command \"%s\", needs \"get\" or \"set\" ...\n",
            argv[2]);
      return 1;
   }
}

