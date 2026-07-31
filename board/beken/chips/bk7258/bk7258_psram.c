/****************************************************************************
 * BK7258 CP-managed PSRAM allocators.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/mm/mm.h>
#include <nuttx/mutex.h>

#include "bk7258_psram.h"
#include "hardware/bk7258_psram.h"

struct bk7258_psram_region
{
  const char *name;
  uintptr_t base;
  size_t size;
};

static const struct bk7258_psram_region g_pool_region[] =
{
  {"psram-user", BK7258_PSRAM_SLAB_USER_BASE,
   BK7258_PSRAM_SLAB_USER_SIZE},
  {"psram-audio", BK7258_PSRAM_SLAB_AUDIO_BASE,
   BK7258_PSRAM_SLAB_AUDIO_SIZE},
  {"psram-encode", BK7258_PSRAM_SLAB_ENCODE_BASE,
   BK7258_PSRAM_SLAB_ENCODE_SIZE},
  {"psram-display", BK7258_PSRAM_SLAB_DISPLAY_BASE,
   BK7258_PSRAM_SLAB_DISPLAY_SIZE},
};

static mutex_t g_psram_lock = NXMUTEX_INITIALIZER;
static struct mm_heap_s *g_psram_heap;
static struct mm_heap_s *g_pool_heap[BK7258_PSRAM_POOL_COUNT];
static uint32_t g_heap_allocations;
static uint32_t g_heap_failures;
static uint32_t g_pool_allocations[BK7258_PSRAM_POOL_COUNT];
static uint32_t g_pool_failures[BK7258_PSRAM_POOL_COUNT];
static uint32_t g_generation;
static enum bk7258_psram_state g_state = BK7258_PSRAM_OFF;
static int g_last_error;

static bool range_contains(uintptr_t base, size_t region_size,
                           const void *ptr, size_t size)
{
  uintptr_t address = (uintptr_t)ptr;
  uintptr_t end = base + region_size;

  return address >= base && address < end && size <= end - address;
}

static bool alignment_valid(size_t alignment)
{
  return alignment >= sizeof(uintptr_t) &&
         (alignment & (alignment - 1u)) == 0;
}

static int psram_probe(void)
{
  static const uintptr_t addresses[] =
  {
    BK7258_PSRAM_SLAB_USER_BASE,
    BK7258_PSRAM_SLAB_USER_BASE + BK7258_PSRAM_SLAB_USER_SIZE - 4u,
    BK7258_PSRAM_SLAB_AUDIO_BASE,
    BK7258_PSRAM_SLAB_AUDIO_BASE + BK7258_PSRAM_SLAB_AUDIO_SIZE - 4u,
    BK7258_PSRAM_SLAB_ENCODE_BASE,
    BK7258_PSRAM_SLAB_ENCODE_BASE + BK7258_PSRAM_SLAB_ENCODE_SIZE - 4u,
    BK7258_PSRAM_SLAB_DISPLAY_BASE,
    BK7258_PSRAM_BASE + 0x00400000u,
    BK7258_PSRAM_SLAB_DISPLAY_BASE + BK7258_PSRAM_SLAB_DISPLAY_SIZE - 4u,
    BK7258_AP_PSRAM_HEAP_BASE,
    BK7258_PSRAM_BASE + 0x00800000u,
    BK7258_AP_PSRAM_HEAP_BASE + BK7258_AP_PSRAM_HEAP_SIZE - 4u,
    BK7258_AP_PSRAM_SECTION_BASE,
    BK7258_PSRAM_BASE + 0x00c00000u,
    BK7258_AP_PSRAM_SECTION_BASE + BK7258_AP_PSRAM_SECTION_SIZE - 4u,
  };
  uint32_t saved[sizeof(addresses) / sizeof(addresses[0])];
  size_t i;
  int ret = OK;

  for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
      saved[i] = *(volatile uint32_t *)addresses[i];
    }

  for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
      *(volatile uint32_t *)addresses[i] =
        0x72580000u ^ (uint32_t)(i * 0x01010101u);
    }

  __asm__ volatile("dmb sy" ::: "memory");
  for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
      volatile uint32_t *word = (volatile uint32_t *)addresses[i];
      uint32_t expected = 0x72580000u ^ (uint32_t)(i * 0x01010101u);

      if (*word != expected)
        {
          ret = -EIO;
          break;
        }
    }

  for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
      *(volatile uint32_t *)addresses[i] = saved[i];
    }

  __asm__ volatile("dmb sy" ::: "memory");
  return ret;
}

static void heap_info(struct bk7258_psram_heap_info *info,
                      struct mm_heap_s *heap, uintptr_t base, size_t size,
                      uint32_t allocations, uint32_t failures)
{
  struct mallinfo mall = mm_mallinfo(heap);

  info->base = base;
  info->size = size;
  info->arena = mall.arena;
  info->allocated = mall.uordblks;
  info->free = mall.fordblks;
  info->largest_free = mall.mxordblk;
  info->peak_allocated = mall.usmblks;
  info->allocation_count = allocations;
  info->failed_allocations = failures;
}

int bk7258_psram_initialize(void)
{
  unsigned int i = 0;
  int ret;

  nxmutex_lock(&g_psram_lock);
  if (g_state == BK7258_PSRAM_ONLINE)
    {
      nxmutex_unlock(&g_psram_lock);
      return OK;
    }

  g_state = BK7258_PSRAM_PROBING;
  printf("psram: probing configured region boundaries\n");
  ret = psram_probe();
  if (ret < 0)
    {
      goto fail;
    }

  printf("psram: boundary probe complete\n");
  for (i = 0; i < BK7258_PSRAM_POOL_COUNT; i++)
    {
      printf("psram: initializing %s pool\n", g_pool_region[i].name);
      g_pool_heap[i] = mm_initialize(g_pool_region[i].name,
                                     (void *)g_pool_region[i].base,
                                     g_pool_region[i].size);
      if (g_pool_heap[i] == NULL)
        {
          ret = -ENOMEM;
          goto fail;
        }
    }

  printf("psram: initializing AP heap\n");
  g_psram_heap = mm_initialize("bk7258-ap-psram",
                               (void *)BK7258_AP_PSRAM_HEAP_BASE,
                               BK7258_AP_PSRAM_HEAP_SIZE);
  if (g_psram_heap == NULL)
    {
      ret = -ENOMEM;
      goto fail;
    }

  g_generation++;
  g_last_error = OK;
  g_state = BK7258_PSRAM_ONLINE;
  nxmutex_unlock(&g_psram_lock);
  return OK;

fail:
  while (i > 0 && i <= BK7258_PSRAM_POOL_COUNT)
    {
      i--;
      mm_uninitialize(g_pool_heap[i]);
      g_pool_heap[i] = NULL;
    }

  g_last_error = ret;
  g_state = BK7258_PSRAM_FAILED;
  nxmutex_unlock(&g_psram_lock);
  return ret;
}

int bk7258_psram_shutdown(void)
{
  unsigned int i;

  nxmutex_lock(&g_psram_lock);
  if (g_state != BK7258_PSRAM_ONLINE)
    {
      nxmutex_unlock(&g_psram_lock);
      return -ENODEV;
    }

  g_state = BK7258_PSRAM_FREEZING;
  if (g_heap_allocations != 0)
    {
      g_state = BK7258_PSRAM_ONLINE;
      nxmutex_unlock(&g_psram_lock);
      return -EBUSY;
    }

  for (i = 0; i < BK7258_PSRAM_POOL_COUNT; i++)
    {
      if (g_pool_allocations[i] != 0)
        {
          g_state = BK7258_PSRAM_ONLINE;
          nxmutex_unlock(&g_psram_lock);
          return -EBUSY;
        }
    }

  mm_uninitialize(g_psram_heap);
  g_psram_heap = NULL;
  for (i = 0; i < BK7258_PSRAM_POOL_COUNT; i++)
    {
      mm_uninitialize(g_pool_heap[i]);
      g_pool_heap[i] = NULL;
    }

  g_state = BK7258_PSRAM_OFFLINE;
  nxmutex_unlock(&g_psram_lock);
  return OK;
}

void bk7258_psram_power_lost(void)
{
  nxmutex_lock(&g_psram_lock);
  g_psram_heap = NULL;
  memset(g_pool_heap, 0, sizeof(g_pool_heap));
  g_heap_allocations = 0;
  memset(g_pool_allocations, 0, sizeof(g_pool_allocations));
  g_generation++;
  g_last_error = -ENODEV;
  g_state = BK7258_PSRAM_FAILED;
  nxmutex_unlock(&g_psram_lock);
}

bool bk7258_psram_is_online(void)
{
  bool online;

  nxmutex_lock(&g_psram_lock);
  online = g_state == BK7258_PSRAM_ONLINE;
  nxmutex_unlock(&g_psram_lock);
  return online;
}

bool bk7258_psram_contains(const void *ptr, size_t size)
{
  unsigned int i;

  if (ptr == NULL)
    {
      return false;
    }

  for (i = 0; i < BK7258_PSRAM_POOL_COUNT; i++)
    {
      if (range_contains(g_pool_region[i].base, g_pool_region[i].size,
                         ptr, size))
        {
          return true;
        }
    }

  return range_contains(BK7258_AP_PSRAM_HEAP_BASE,
                        BK7258_AP_PSRAM_HEAP_SIZE, ptr, size) ||
         range_contains(BK7258_AP_PSRAM_SECTION_BASE,
                        BK7258_AP_PSRAM_SECTION_SIZE, ptr, size);
}

void *bk7258_psram_malloc(size_t size)
{
  void *ptr = NULL;

  nxmutex_lock(&g_psram_lock);
  if (g_state == BK7258_PSRAM_ONLINE)
    {
      ptr = mm_malloc(g_psram_heap, size);
      if (ptr != NULL)
        {
          g_heap_allocations++;
        }
      else
        {
          g_heap_failures++;
        }
    }
  else
    {
      set_errno(ENODEV);
    }

  nxmutex_unlock(&g_psram_lock);
  return ptr;
}

void *bk7258_psram_zalloc(size_t size)
{
  void *ptr = bk7258_psram_malloc(size);
  if (ptr != NULL)
    {
      memset(ptr, 0, size);
    }

  return ptr;
}

void *bk7258_psram_calloc(size_t n, size_t elem_size)
{
  if (elem_size != 0 && n > SIZE_MAX / elem_size)
    {
      set_errno(ENOMEM);
      return NULL;
    }

  return bk7258_psram_zalloc(n * elem_size);
}

void *bk7258_psram_realloc(void *ptr, size_t size)
{
  void *result = NULL;

  if (ptr == NULL)
    {
      return bk7258_psram_malloc(size);
    }

  nxmutex_lock(&g_psram_lock);
  if (g_state != BK7258_PSRAM_ONLINE ||
      !range_contains(BK7258_AP_PSRAM_HEAP_BASE,
                      BK7258_AP_PSRAM_HEAP_SIZE, ptr, 1))
    {
      set_errno(g_state == BK7258_PSRAM_ONLINE ? EINVAL : ENODEV);
    }
  else
    {
      result = mm_realloc(g_psram_heap, ptr, size);
      if (result == NULL && size != 0)
        {
          g_heap_failures++;
        }
      else if (size == 0)
        {
          g_heap_allocations--;
        }
    }

  nxmutex_unlock(&g_psram_lock);
  return result;
}

void *bk7258_psram_memalign(size_t alignment, size_t size)
{
  void *ptr = NULL;

  if (!alignment_valid(alignment))
    {
      set_errno(EINVAL);
      return NULL;
    }

  nxmutex_lock(&g_psram_lock);
  if (g_state == BK7258_PSRAM_ONLINE)
    {
      ptr = mm_memalign(g_psram_heap, alignment, size);
      if (ptr != NULL)
        {
          g_heap_allocations++;
        }
      else
        {
          g_heap_failures++;
        }
    }
  else
    {
      set_errno(ENODEV);
    }

  nxmutex_unlock(&g_psram_lock);
  return ptr;
}

void bk7258_psram_free(void *ptr)
{
  if (ptr == NULL)
    {
      return;
    }

  nxmutex_lock(&g_psram_lock);
  if (g_state == BK7258_PSRAM_ONLINE &&
      range_contains(BK7258_AP_PSRAM_HEAP_BASE,
                     BK7258_AP_PSRAM_HEAP_SIZE, ptr, 1))
    {
      mm_free(g_psram_heap, ptr);
      g_heap_allocations--;
    }
  else
    {
      set_errno(g_state == BK7258_PSRAM_ONLINE ? EINVAL : ENODEV);
    }

  nxmutex_unlock(&g_psram_lock);
}

void *bk7258_media_pool_alloc(enum bk7258_psram_pool pool,
                              size_t alignment, size_t size)
{
  void *ptr = NULL;

  if ((unsigned int)pool >= BK7258_PSRAM_POOL_COUNT ||
      (alignment != 0 && !alignment_valid(alignment)))
    {
      set_errno(EINVAL);
      return NULL;
    }

  nxmutex_lock(&g_psram_lock);
  if (g_state == BK7258_PSRAM_ONLINE)
    {
      ptr = alignment == 0 ? mm_malloc(g_pool_heap[pool], size) :
                             mm_memalign(g_pool_heap[pool], alignment, size);
      if (ptr != NULL)
        {
          g_pool_allocations[pool]++;
        }
      else
        {
          g_pool_failures[pool]++;
        }
    }
  else
    {
      set_errno(ENODEV);
    }

  nxmutex_unlock(&g_psram_lock);
  return ptr;
}

void bk7258_media_pool_free(enum bk7258_psram_pool pool, void *ptr)
{
  if (ptr == NULL)
    {
      return;
    }

  nxmutex_lock(&g_psram_lock);
  if ((unsigned int)pool < BK7258_PSRAM_POOL_COUNT &&
      g_state == BK7258_PSRAM_ONLINE &&
      range_contains(g_pool_region[pool].base, g_pool_region[pool].size,
                     ptr, 1))
    {
      mm_free(g_pool_heap[pool], ptr);
      g_pool_allocations[pool]--;
    }
  else
    {
      set_errno(g_state == BK7258_PSRAM_ONLINE ? EINVAL : ENODEV);
    }

  nxmutex_unlock(&g_psram_lock);
}

int bk7258_psram_info(struct bk7258_psram_info *info)
{
  unsigned int i;

  if (info == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_psram_lock);
  memset(info, 0, sizeof(*info));
  info->base = BK7258_PSRAM_BASE;
  info->capacity = BK7258_PSRAM_SIZE;
  info->generation = g_generation;
  info->state = g_state;
  info->last_error = g_last_error;
  info->heap.base = BK7258_AP_PSRAM_HEAP_BASE;
  info->heap.size = BK7258_AP_PSRAM_HEAP_SIZE;
  for (i = 0; i < BK7258_PSRAM_POOL_COUNT; i++)
    {
      info->pool[i].base = g_pool_region[i].base;
      info->pool[i].size = g_pool_region[i].size;
    }

  if (g_state == BK7258_PSRAM_ONLINE)
    {
      heap_info(&info->heap, g_psram_heap, BK7258_AP_PSRAM_HEAP_BASE,
                BK7258_AP_PSRAM_HEAP_SIZE, g_heap_allocations,
                g_heap_failures);
      for (i = 0; i < BK7258_PSRAM_POOL_COUNT; i++)
        {
          heap_info(&info->pool[i], g_pool_heap[i], g_pool_region[i].base,
                    g_pool_region[i].size, g_pool_allocations[i],
                    g_pool_failures[i]);
        }
    }

  nxmutex_unlock(&g_psram_lock);
  return OK;
}

void bk7258_psram_dump(void)
{
  static const char * const names[] =
  {
    "user", "audio", "encode", "display"
  };
  struct bk7258_psram_info info;
  unsigned int i;

  if (bk7258_psram_info(&info) < 0)
    {
      return;
    }

  printf("psram: state=%lu generation=%lu error=%d "
         "range=%08lx-%08lx\n",
         (unsigned long)info.state, (unsigned long)info.generation,
         info.last_error, (unsigned long)info.base,
         (unsigned long)(info.base + info.capacity - 1u));
  printf("psram: ap heap allocated=%lu free=%lu largest=%lu count=%lu "
         "failures=%lu\n", (unsigned long)info.heap.allocated,
         (unsigned long)info.heap.free,
         (unsigned long)info.heap.largest_free,
         (unsigned long)info.heap.allocation_count,
         (unsigned long)info.heap.failed_allocations);
  for (i = 0; i < BK7258_PSRAM_POOL_COUNT; i++)
    {
      printf("psram: %s pool allocated=%lu free=%lu largest=%lu "
             "count=%lu failures=%lu\n", names[i],
             (unsigned long)info.pool[i].allocated,
             (unsigned long)info.pool[i].free,
             (unsigned long)info.pool[i].largest_free,
             (unsigned long)info.pool[i].allocation_count,
             (unsigned long)info.pool[i].failed_allocations);
    }
}

uint32_t bk7258_psram_heap_used(void)
{
  uint32_t used = 0;

  nxmutex_lock(&g_psram_lock);
  if (g_state == BK7258_PSRAM_ONLINE)
    {
      used = mm_mallinfo(g_psram_heap).uordblks;
    }

  nxmutex_unlock(&g_psram_lock);
  return used;
}
