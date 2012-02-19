#define _GNU_SOURCE

#include "control.h"
#include "cuse-mix.h"
#include <sys/socket.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/un.h>

static int g_epfd;
static pthread_t g_thread;
static int listen_fd;

static void accept_connection(void)
{
   int fd = accept(listen_fd, NULL, NULL);
   if (fd < 0)
   {
      perror("accept");
      return;
   }

   if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)
   {
      perror("fcntl");
      close(fd);
      return;
   }

   if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd,
         &(struct epoll_event) {
            .events = POLLIN,
            .data = {
               .fd = fd
            },
         }) < 0)
   {
      perror("epoll_ctl");
      close(fd);
   }
}

static void request_reply(int fd, const char *str)
{
   char msg[256];
   snprintf(msg, sizeof(msg), "MARU%4zu %s", strlen(str) + 1, str);
   ssize_t len = strlen(msg);
   if (write(fd, msg, len) != len)
   {
      fprintf(stderr, "Failed to write ...\n");
      close(fd);
   }
}

static void request_setplayvol(int fd, int argc, char *argv[])
{
   if (argc < 2)
      return;

   errno = 0;
   unsigned stream = strtoul(argv[0], NULL, 0);
   int vol = strtol(argv[1], NULL, 0);

   if (errno)
      return request_reply(fd, "NAK");

   if (stream >= MAX_STREAMS)
      return request_reply(fd, "NAK");

   if (vol < 0 || vol > 100)
      return request_reply(fd, "NAK");

   const char *reply;
   global_lock();

   if (g_state.stream_info[stream].active)
   {
      g_state.stream_info[stream].volume = vol;
      g_state.stream_info[stream].volume_f = vol / 100.0f;
      reply = "ACK";
   }
   else
      reply = "NOSTREAM";

   global_unlock();

   request_reply(fd, reply);
}

static void request_getplayvol(int fd, int argc, char *argv[])
{
   if (argc < 1)
      return;

   errno = 0;
   unsigned stream = strtoul(argv[0], NULL, 0);
   if (errno)
      return request_reply(fd, "NAK");

   if (stream >= MAX_STREAMS)
      return request_reply(fd, "NAK");

   char vol[16] = "NOSTREAM";

   global_lock();

   if (g_state.stream_info[stream].active)
      snprintf(vol, sizeof(vol), "%d", g_state.stream_info[stream].volume);

   global_unlock();

   request_reply(fd, vol);
}

static void parse_request(int fd, int argc, char *argv[])
{
   if (!argc)
      return;

   if (strcmp(argv[0], "SETPLAYVOL") == 0)
      request_setplayvol(fd, argc - 1, argv + 1);
   else if (strcmp(argv[0], "GETPLAYVOL") == 0)
      request_getplayvol(fd, argc - 1, argv + 1);
   else
      fprintf(stderr, "Invalid request!\n");
}

static void handle_request(int fd)
{
   char req_header[8];
   ssize_t ret = read(fd, req_header, sizeof(req_header));
   if (ret < (ssize_t)req_header)
   {
      close(fd);
      return;
   }

   size_t maru_len = strlen("MARU ");
   char *substr = memmem(req_header, sizeof(req_header),
         "MARU ", maru_len);

   if (!substr)
   {
      fprintf(stderr, "Invalid proto header!\n");
      return;
   }
   substr += maru_len;

   errno = 0;
   unsigned request_len = strtoul(substr, NULL, 10);
   if (errno)
   {
      fprintf(stderr, "Invalid length!\n");
      return;
   }

#define REQUEST_MAX_LEN 255
   if (request_len > REQUEST_MAX_LEN)
   {
      fprintf(stderr, "Invalid length!\n");
      return;
   }

   char request[REQUEST_MAX_LEN + 1];

   ret = read(fd, request, request_len);
   if (ret < (ssize_t)request_len)
   {
      fprintf(stderr, "Couldn't read complete request.\n");
      return;
   }
   request[ret] = '\0';

   int argc = 0;
   char *argv[REQUEST_MAX_LEN];
   char *tok;

   argv[argc] = strtok_r(request, " ", &tok);
   while (argv[argc])
   {
      argc++;
      argv[argc] = strtok_r(NULL, "\n", &tok);
   }

   parse_request(fd, argc, argv);
}

static void *thread_entry(void *data)
{
   (void)data;

   sigaction(SIGPIPE, &(const struct sigaction) { .sa_handler = SIG_IGN }, NULL);

   for (;;)
   {
      int ret;
      struct epoll_event events[16];
poll_retry:
      ret = epoll_wait(g_epfd, events, 16, -1);

      if (ret < 0)
      {
         if (errno == EINTR)
            goto poll_retry;

         printf("epoll_wait failed!\n");
         exit(1);
      }

      for (int i = 0; i < ret; i++)
      {
         if (events[i].data.fd == listen_fd)
            accept_connection();
         else
            handle_request(events[i].data.fd);
      }
   }

   return NULL;
}

static int create_unix_socket(const char *path)
{
   struct sockaddr_un un;
   memset(&un, 0, sizeof(un));

   un.sun_family = AF_UNIX;
   strncpy(un.sun_path, path, sizeof(un.sun_path));

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;

   unlink(path);

   if (bind(fd, (struct sockaddr*)&un, sizeof(un)) < 0)
   {
      close(fd);
      return -1;
   }

   if (listen(fd, 4) < 0)
   {
      close(fd);
      return -1;
   }

   return fd;
}

bool start_control_thread(void)
{
   g_epfd = epoll_create(16);
   if (g_epfd < 0)
   {
      perror("epoll_create");
      return false;
   }

   listen_fd = create_unix_socket("/tmp/marumix");
   if (listen_fd < 0)
      return false;

   if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, listen_fd,
            &(struct epoll_event) {
               .events = POLLIN,
               .data = {
                  .fd = listen_fd
               }
            }) < 0)
   {
      perror("epoll_ctl");
      return false;
   }

   if (pthread_create(&g_thread, NULL, thread_entry, NULL) < 0)
   {
      perror("pthread_create");
      return false;
   }

   return true;
}

