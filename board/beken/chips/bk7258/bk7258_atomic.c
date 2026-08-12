/****************************************************************************
 * BK7258 libc atomic hwspinlock instance.
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/hwspinlock/hwspinlock.h>

extern const struct hwspinlock_ops_s g_bk7258_hwspinlock_ops;

struct hwspinlock_dev_s g_atomic_hwspinlock =
{
  .id       = 0,
  .priority = 0,
  .ops      = &g_bk7258_hwspinlock_ops
};
