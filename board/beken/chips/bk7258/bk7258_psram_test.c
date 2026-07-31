/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/bk7258_psram.h"

#define BK7258_PSRAM_TEST_PATTERN0 0xaaaaaaaa
#define BK7258_PSRAM_TEST_PATTERN1 0x55555555

struct bk7258_psram_test_word
{
  uintptr_t address;
  uint32_t saved;
};

static const uintptr_t g_bk7258_psram_test_addresses[] =
{
  BK7258_PSRAM_SLAB_USER_BASE,
  BK7258_PSRAM_SLAB_USER_BASE + BK7258_PSRAM_SLAB_USER_SIZE - 4,
  BK7258_PSRAM_SLAB_AUDIO_BASE,
  BK7258_PSRAM_SLAB_AUDIO_BASE + BK7258_PSRAM_SLAB_AUDIO_SIZE - 4,
  BK7258_PSRAM_SLAB_ENCODE_BASE,
  BK7258_PSRAM_SLAB_ENCODE_BASE + BK7258_PSRAM_SLAB_ENCODE_SIZE - 4,
  BK7258_PSRAM_SLAB_DISPLAY_BASE,
  BK7258_PSRAM_SLAB_DISPLAY_BASE + BK7258_PSRAM_SLAB_DISPLAY_SIZE - 4,
  BK7258_AP_PSRAM_HEAP_BASE,
  BK7258_AP_PSRAM_HEAP_BASE + BK7258_AP_PSRAM_HEAP_SIZE - 4,
  BK7258_AP_PSRAM_SECTION_BASE,
  BK7258_AP_PSRAM_SECTION_BASE + BK7258_AP_PSRAM_SECTION_SIZE - 4,
};

static uint32_t bk7258_psram_read(uintptr_t address)
{
  return *(volatile uint32_t *)address;
}

static void bk7258_psram_write(uintptr_t address, uint32_t value)
{
  *(volatile uint32_t *)address = value;
}

int bk7258_psram_test(int argc, char **argv)
{
  struct bk7258_psram_test_word saved[
    sizeof(g_bk7258_psram_test_addresses) /
    sizeof(g_bk7258_psram_test_addresses[0])];
  size_t i;
  uintptr_t alias_a = BK7258_PSRAM_BASE;
  uintptr_t alias_b = BK7258_PSRAM_BASE + 0x00800000u;
  int ret = 0;

  (void)argc;
  (void)argv;

  printf("psram: AP boundary test start, CP heap skipped\n");

  for (i = 0; i < sizeof(saved) / sizeof(saved[0]); i++)
    {
      saved[i].address = g_bk7258_psram_test_addresses[i];
      saved[i].saved = bk7258_psram_read(saved[i].address);
    }

  for (i = 0; i < sizeof(saved) / sizeof(saved[0]); i++)
    {
      bk7258_psram_write(saved[i].address, BK7258_PSRAM_TEST_PATTERN0);
      if (bk7258_psram_read(saved[i].address) !=
          BK7258_PSRAM_TEST_PATTERN0)
        {
          printf("psram: read/write failed at 0x%08lx\n",
                 (unsigned long)saved[i].address);
          ret = -1;
          break;
        }

      bk7258_psram_write(saved[i].address, BK7258_PSRAM_TEST_PATTERN1);
      if (bk7258_psram_read(saved[i].address) !=
          BK7258_PSRAM_TEST_PATTERN1)
        {
          printf("psram: pattern failed at 0x%08lx\n",
                 (unsigned long)saved[i].address);
          ret = -1;
          break;
        }
    }

  if (ret == 0)
    {
      uint32_t value_a = bk7258_psram_read(alias_a);
      uint32_t value_b = bk7258_psram_read(alias_b);

      bk7258_psram_write(alias_a, BK7258_PSRAM_TEST_PATTERN0);
      bk7258_psram_write(alias_b, BK7258_PSRAM_TEST_PATTERN1);
      if (bk7258_psram_read(alias_a) != BK7258_PSRAM_TEST_PATTERN0 ||
          bk7258_psram_read(alias_b) != BK7258_PSRAM_TEST_PATTERN1)
        {
          printf("psram: 8 MB alias test failed\n");
          ret = -1;
        }

      bk7258_psram_write(alias_a, value_a);
      bk7258_psram_write(alias_b, value_b);
    }

  for (i = 0; i < sizeof(saved) / sizeof(saved[0]); i++)
    {
      bk7258_psram_write(saved[i].address, saved[i].saved);
    }

  if (ret == 0)
    {
      printf("psram: AP boundary and 8 MB alias tests passed\n");
    }
  else
    {
      printf("psram: AP test failed; allocator must remain offline\n");
    }

  return ret;
}
