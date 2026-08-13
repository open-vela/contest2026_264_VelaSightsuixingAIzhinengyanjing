/****************************************************************************
 * BK7258 peripheral self-test.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <net/if.h>

#include <nuttx/clock.h>
#include <arch/chip/bk7258_psram.h>

static unsigned int test_proc_file(const char *path)
{
  char buffer[128];
  int fd;
  ssize_t n;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      printf("FAIL %s open errno=%d\n", path, errno);
      return 1;
    }

  n = read(fd, buffer, sizeof(buffer));
  close(fd);
  if (n <= 0)
    {
      printf("FAIL %s read=%ld\n", path, (long)n);
      return 1;
    }

  printf("PASS %s bytes=%ld\n", path, (long)n);
  return 0;
}

static unsigned int test_psram(void)
{
  static const unsigned char seed = 0xa5;
  unsigned char buffer[256];
  struct bk7258_psram_info info;
  unsigned int i;
  void *memory;

  if (!bk7258_psram_is_online() || bk7258_psram_info(&info) < 0 ||
      info.base != 0x60000000u || info.capacity != 0x01000000u)
    {
      printf("FAIL psram online/info\n");
      return 1;
    }

  memory = bk7258_psram_memalign(32, sizeof(buffer));
  if (memory == NULL || !bk7258_psram_contains(memory, sizeof(buffer)))
    {
      printf("FAIL psram allocation\n");
      if (memory != NULL)
        {
          bk7258_psram_free(memory);
        }
      return 1;
    }

  for (i = 0; i < sizeof(buffer); i++)
    {
      buffer[i] = (unsigned char)(seed ^ i);
    }
  memcpy(memory, buffer, sizeof(buffer));
  memset(buffer, 0, sizeof(buffer));
  memcpy(buffer, memory, sizeof(buffer));
  bk7258_psram_free(memory);

  for (i = 0; i < sizeof(buffer); i++)
    {
      if (buffer[i] != (unsigned char)(seed ^ i))
        {
          printf("FAIL psram read/write index=%u\n", i);
          return 1;
        }
    }

  printf("PASS psram base=0x%08lx size=%lu\n",
         (unsigned long)info.base, (unsigned long)info.capacity);
  return 0;
}

static unsigned int test_uart(void)
{
  static const char message[] = "periph_selftest: mailbox UART TX\n";
  int fd;
  ssize_t written;

  fd = open("/dev/ttyMB0", O_WRONLY);
  if (fd < 0)
    {
      printf("FAIL /dev/ttyMB0 open errno=%d\n", errno);
      return 1;
    }

  written = write(fd, message, sizeof(message) - 1);
  if (written == (ssize_t)(sizeof(message) - 1))
    {
      (void)tcdrain(fd);
    }
  close(fd);
  if (written != (ssize_t)(sizeof(message) - 1))
    {
      printf("FAIL /dev/ttyMB0 write=%ld\n", (long)written);
      return 1;
    }

  printf("PASS mailbox UART TX\n");
  return 0;
}

int periph_selftest_main(int argc, char *argv[])
{
  clock_t before;
  clock_t after;
  unsigned int failures = 0;

  (void)argc;
  (void)argv;
  printf("SELFTEST BEGIN\n");

  if (if_nametoindex("wlan0") == 0)
    {
      printf("FAIL wlan0 interface missing\n");
      failures++;
    }
  else
    {
      printf("PASS wlan0 interface present\n");
    }

  failures += test_uart();
  failures += test_psram();
  failures += test_proc_file("/proc/version");
  failures += test_proc_file("/proc/uptime");
  failures += test_proc_file("/proc/meminfo");
  before = clock_systime_ticks();
  usleep(10000);
  after = clock_systime_ticks();
  if (after == before)
    {
      printf("FAIL systick did not advance\n");
      failures++;
    }
  else
    {
      printf("PASS systick\n");
    }

  printf("Wi-Fi separate test: ifup wlan0; wapi scan wlan0; ifconfig wlan0\n");
  printf("SELFTEST %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
  return failures == 0 ? 0 : 1;
}
