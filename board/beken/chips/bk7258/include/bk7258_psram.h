/****************************************************************************
 * vendor/beken/chips/bk7258/include/bk7258_psram.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_PSRAM_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_PSRAM_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum bk7258_psram_pool
{
  BK7258_PSRAM_POOL_USER = 0,
  BK7258_PSRAM_POOL_AUDIO,
  BK7258_PSRAM_POOL_ENCODE,
  BK7258_PSRAM_POOL_DISPLAY,
  BK7258_PSRAM_POOL_COUNT
};

enum bk7258_psram_state
{
  BK7258_PSRAM_OFF = 0,
  BK7258_PSRAM_POWER_REQUESTED,
  BK7258_PSRAM_PROBING,
  BK7258_PSRAM_ONLINE,
  BK7258_PSRAM_FREEZING,
  BK7258_PSRAM_OFFLINE,
  BK7258_PSRAM_RECOVERING,
  BK7258_PSRAM_FAILED
};

struct bk7258_psram_heap_info
{
  uintptr_t base;
  size_t size;
  size_t arena;
  size_t allocated;
  size_t free;
  size_t largest_free;
  size_t peak_allocated;
  uint32_t allocation_count;
  uint32_t failed_allocations;
};

struct bk7258_psram_info
{
  uintptr_t base;
  size_t capacity;
  uint32_t generation;
  uint32_t state;
  int last_error;
  struct bk7258_psram_heap_info heap;
  struct bk7258_psram_heap_info pool[BK7258_PSRAM_POOL_COUNT];
};

#ifdef CONFIG_BK7258_PSRAM
int bk7258_psram_initialize(void);
int bk7258_psram_shutdown(void);
void bk7258_psram_power_lost(void);
bool bk7258_psram_is_online(void);
bool bk7258_psram_contains(const void *ptr, size_t size);

void *bk7258_psram_malloc(size_t size);
void *bk7258_psram_zalloc(size_t size);
void *bk7258_psram_calloc(size_t n, size_t elem_size);
void *bk7258_psram_realloc(void *ptr, size_t size);
void *bk7258_psram_memalign(size_t alignment, size_t size);
void bk7258_psram_free(void *ptr);

void *bk7258_media_pool_alloc(enum bk7258_psram_pool pool,
                              size_t alignment, size_t size);
void bk7258_media_pool_free(enum bk7258_psram_pool pool, void *ptr);

int bk7258_psram_info(struct bk7258_psram_info *info);
void bk7258_psram_dump(void);
uint32_t bk7258_psram_heap_used(void);
#endif

#endif
