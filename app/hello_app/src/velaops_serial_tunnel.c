/****************************************************************************
 * Contest 2026 team 023 - VelaOps 串口 LLM 隧道（板端实现）。
 ****************************************************************************/

#ifdef __NuttX__

#include "velaops_serial_tunnel.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define VELAOPS_TUNNEL_IN_B64 "/tmp/vop-in.b64"
#define VELAOPS_TUNNEL_IN_READY "/tmp/vop-in.ready"
#define VELAOPS_TUNNEL_REQ_MAX (64 * 1024)
#define VELAOPS_TUNNEL_RESP_MAX (256 * 1024)
/* 发帧后等待 relay 响应的秒数；不重发（重发会被 Proxy 判为 replay_detected）。 */
#define VELAOPS_TUNNEL_WAIT_SECONDS 12
#define VELAOPS_TUNNEL_EMIT_ATTEMPTS 1

static const char g_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 输出 base64 帧；一帧一次 write，尽量降低与日志交错导致的断行。 */
static int velaops_tunnel_emit(const char *data, size_t len)
{
  size_t out_cap = 4 * ((len + 2) / 3) + 64;
  char *out = malloc(out_cap);
  size_t i;
  size_t o = 0;

  if (out == NULL)
    {
      return -1;
    }

  /* 单行整帧：前置换行先收尾其它线程可能未换行的日志，再一次 write 发出。 */
  o += (size_t)snprintf(out + o, out_cap - o, "\n@@VOPREQ ");
  for (i = 0; i < len; i += 3)
    {
      unsigned int v = (unsigned int)(unsigned char)data[i] << 16;

      if (i + 1 < len)
        {
          v |= (unsigned int)(unsigned char)data[i + 1] << 8;
        }
      if (i + 2 < len)
        {
          v |= (unsigned int)(unsigned char)data[i + 2];
        }

      out[o++] = g_b64[(v >> 18) & 0x3f];
      out[o++] = g_b64[(v >> 12) & 0x3f];
      out[o++] = i + 1 < len ? g_b64[(v >> 6) & 0x3f] : '=';
      out[o++] = i + 2 < len ? g_b64[v & 0x3f] : '=';
    }
  out[o++] = '\n';

  if (write(STDOUT_FILENO, out, o) != (ssize_t)o)
    {
      free(out);
      return -1;
    }
  free(out);
  return 0;
}

static int velaops_tunnel_b64_value(char c)
{
  const char *p = strchr(g_b64, c);

  return p == NULL ? -1 : (int)(p - g_b64);
}

/* 解码 base64 文件到动态缓冲；返回字节数或 -1。 */
static ssize_t velaops_tunnel_load_response(char **out)
{
  FILE *file;
  long size;
  char *b64;
  char *data;
  size_t i;
  size_t o = 0;

  file = fopen(VELAOPS_TUNNEL_IN_B64, "r");
  if (file == NULL)
    {
      return -1;
    }
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size <= 0 || size > VELAOPS_TUNNEL_RESP_MAX)
    {
      fclose(file);
      return -1;
    }

  b64 = malloc((size_t)size + 1);
  data = malloc((size_t)size + 1);
  if (b64 == NULL || data == NULL)
    {
      free(b64);
      free(data);
      fclose(file);
      return -1;
    }
  if (fread(b64, 1, (size_t)size, file) != (size_t)size)
    {
      free(b64);
      free(data);
      fclose(file);
      return -1;
    }
  fclose(file);
  b64[size] = '\0';

  for (i = 0; i < (size_t)size; i++)
    {
      int a;
      int b;
      int c;
      int d;
      unsigned int v;

      if (b64[i] == '=' || b64[i] == '\n' || b64[i] == '\r' ||
          b64[i] == ' ' || b64[i] == '\t')
        {
          continue;
        }
      a = velaops_tunnel_b64_value(b64[i]);
      b = (i + 1 < (size_t)size) ? velaops_tunnel_b64_value(b64[i + 1]) : -1;
      c = (i + 2 < (size_t)size) ? velaops_tunnel_b64_value(b64[i + 2]) : -1;
      d = (i + 3 < (size_t)size) ? velaops_tunnel_b64_value(b64[i + 3]) : -1;
      if (a < 0 || b < 0)
        {
          break;
        }
      v = ((unsigned int)a << 18) | ((unsigned int)b << 12) |
          ((c < 0 ? 0u : (unsigned int)c) << 6) |
          (d < 0 ? 0u : (unsigned int)d);
      data[o++] = (char)((v >> 16) & 0xff);
      if (c >= 0)
        {
          data[o++] = (char)((v >> 8) & 0xff);
        }
      if (d >= 0)
        {
          data[o++] = (char)(v & 0xff);
        }
      i += 3;
    }

  free(b64);
  *out = data;
  return (ssize_t)o;
}

/* 读取一个完整 HTTP 请求（依赖 Content-Length）。成功返回 0。 */
static int velaops_tunnel_read_request(int fd, char **out, size_t *out_len)
{
  char *buf = malloc(8192);
  size_t cap = 8192;
  size_t len = 0;
  size_t header_end = 0;
  long content_len = -1;

  if (buf == NULL)
    {
      return -1;
    }

  for (;;)
    {
      ssize_t n;

      if (len + 1 >= cap && cap < VELAOPS_TUNNEL_REQ_MAX)
        {
          char *grown;
          size_t new_cap = cap * 2 > VELAOPS_TUNNEL_REQ_MAX
                               ? VELAOPS_TUNNEL_REQ_MAX
                               : cap * 2;

          grown = realloc(buf, new_cap);
          if (grown == NULL)
            {
              free(buf);
              return -1;
            }
          buf = grown;
          cap = new_cap;
        }
      if (len + 1 >= cap)
        {
          free(buf);
          return -1;
        }

      n = recv(fd, buf + len, cap - 1 - len, 0);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }
      if (n < 0)
        {
          free(buf);
          return -1;
        }
      if (n == 0)
        {
          break;
        }
      len += (size_t)n;
      buf[len] = '\0';

      if (header_end == 0)
        {
          char *p = strstr(buf, "\r\n\r\n");

          if (p != NULL)
            {
              char *cl;

              header_end = (size_t)(p - buf) + 4;
              cl = strcasestr(buf, "content-length:");
              if (cl != NULL && cl < p)
                {
                  content_len = strtol(cl + strlen("content-length:"),
                                       NULL, 10);
                }
              else
                {
                  content_len = 0;
                }
            }
        }

      if (header_end != 0 && content_len >= 0 &&
          len >= header_end + (size_t)content_len)
        {
          break;
        }
    }

  if (header_end == 0)
    {
      free(buf);
      return -1;
    }
  *out = buf;
  *out_len = len;
  return 0;
}

static void velaops_tunnel_send_error(int fd)
{
  static const char msg[] =
    "HTTP/1.1 504 Gateway Timeout\r\n"
    "Content-Length: 0\r\nConnection: close\r\n\r\n";

  (void)write(fd, msg, sizeof(msg) - 1);
}

int velaops_serial_tunnel_run(void)
{
  struct sockaddr_in addr;
  struct timeval tv;
  int listen_fd;
  int option = 1;
  int bind_err;

  syslog(LOG_ERR, "velaops tunnel: starting\n");

  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0)
    {
      syslog(LOG_ERR, "velaops tunnel: socket failed: %d\n", errno);
      return -1;
    }
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(VELAOPS_TUNNEL_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind_err = bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
  if (bind_err < 0 || listen(listen_fd, 4) < 0)
    {
      syslog(LOG_ERR, "velaops tunnel: bind/listen failed: %d (bind=%d)\n",
             errno, bind_err);
      close(listen_fd);
      return -1;
    }

  syslog(LOG_ERR, "velaops tunnel: listening 127.0.0.1:%d\n",
         VELAOPS_TUNNEL_PORT);

  tv.tv_sec = 30;
  tv.tv_usec = 0;

  for (;;)
    {
      int client = accept(listen_fd, NULL, NULL);
      char *request = NULL;
      size_t request_len = 0;
      char *response = NULL;
      ssize_t response_len;
      int waited;
      int i;

      if (client < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }
          sleep(1);
          continue;
        }
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      if (velaops_tunnel_read_request(client, &request, &request_len) != 0)
        {
          close(client);
          continue;
        }

      /* 发帧并等响应：控制台偶发插帧会让 relay 丢请求，重发最多 3 次。 */
      waited = 0;
      {
        int attempt;

        for (attempt = 0;
             attempt < VELAOPS_TUNNEL_EMIT_ATTEMPTS && !waited; attempt++)
          {
            unlink(VELAOPS_TUNNEL_IN_READY);
            if (velaops_tunnel_emit(request, request_len) != 0)
              {
                break;
              }
            for (i = 0; i < VELAOPS_TUNNEL_WAIT_SECONDS; i++)
              {
                struct stat st;

                if (stat(VELAOPS_TUNNEL_IN_READY, &st) == 0)
                  {
                    waited = 1;
                    break;
                  }
                sleep(1);
              }
          }
        free(request);
      }
      if (!waited)
        {
          syslog(LOG_ERR, "velaops tunnel: relay response timeout\n");
          velaops_tunnel_send_error(client);
          close(client);
          continue;
        }

      response_len = velaops_tunnel_load_response(&response);
      if (response_len > 0)
        {
          size_t sent = 0;

          while (sent < (size_t)response_len)
            {
              ssize_t n = write(client, response + sent,
                                (size_t)response_len - sent);
              if (n <= 0)
                {
                  break;
                }
              sent += (size_t)n;
            }
          syslog(LOG_ERR, "velaops tunnel: wrote LLM response %ld bytes\n",
                 (long)response_len);
        }
      else
        {
          velaops_tunnel_send_error(client);
        }
      if (response != NULL)
        {
          free(response);
        }
      unlink(VELAOPS_TUNNEL_IN_READY);
      unlink(VELAOPS_TUNNEL_IN_B64);
      close(client);
    }
}

#endif /* __NuttX__ */
