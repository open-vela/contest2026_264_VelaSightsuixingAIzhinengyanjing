/****************************************************************************
 * app/velasight/vs_cli.c
 *
 * nsh commands that operate the running UI: what a person does with the three
 * keys, done from the console, and what the two screens then show, read back.
 * So a script -- or an AI on the serial console -- can take the product
 * through a whole feature and check every step of it.
 *
 * Nothing here calls into a feature.  A command injects the gestures the keys
 * would have produced, through vs_app_inject_input() into the queue the key
 * task feeds, and then watches the status the UI thread publishes.  It runs
 * the same handlers, request ids and deadlines a real press does, and it can
 * only reach what a user can: a page that offers no gesture for something
 * gives a command no way to do it either.
 *
 * Every command ends with exactly one line to match on:
 *
 *   vs_cli: ok <command> page=<page> ...
 *   vs_cli: fail <command> page=<page> why="..." ...
 *
 * and "vs_cli: page <page> stage <stage>" lines trace a long wait.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "include/vs_app.h"
#include "include/vs_cli.h"
#include "include/vs_haptic.h"
#include "include/vs_social.h"
#include "include/vs_tts.h"
#include "include/vs_voice.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_VS_LONG_PRESS_MS
#  define CONFIG_VS_LONG_PRESS_MS 2000
#endif

#ifndef CONFIG_VS_COMBO_PRESS_MS
#  define CONFIG_VS_COMBO_PRESS_MS 2000
#endif

#ifndef CONFIG_VS_SOCIAL_FINALIZE_TIMEOUT_MS
#  define CONFIG_VS_SOCIAL_FINALIZE_TIMEOUT_MS 240000
#endif

/* How often a wait reads the status, and how long the UI may take to act on
 * an injected gesture.  The second is generous because the UI thread can be
 * held off for a few hundred milliseconds at a time during a session -- a
 * synchronous panel flush, the camera's entropy validator -- and a command
 * that gave up inside that would report a working UI as a broken one.
 */

#define CLI_POLL_MS   100u
#define CLI_INPUT_MS  5000u

/* Per-step limits.  Each sits past the UI's own deadline for the same step,
 * so a stall is reported by the UI, on its error page and in its words; these
 * only catch a UI that has stopped answering altogether.
 */

#define CLI_START_MS     45000u   /* the starting page allows 40 s */
#define CLI_PAUSE_MS     45000u   /* so do pausing and resuming */
#define CLI_VOICE_MS     30000u
#define CLI_NET_MS       60000u
#define CLI_SETTLE_MS    10000u   /* a saving page, the end of boot */

/* The whole finalize, from the stop gesture to the result page, which is one
 * wait rather than one per stage.  So it is derived from the longest stage,
 * waiting for the minutes, plus two minutes for stopping, closing, saving
 * and fetching the spoken minutes around it.  A fixed figure would turn into
 * the shorter bound the moment that Kconfig value was raised past it.
 */

#define CLI_FINALIZE_MS  (CONFIG_VS_SOCIAL_FINALIZE_TIMEOUT_MS + 120000u)

#define CLI_RUN_SECONDS   30u     /* "social run" with no duration */
#define CLI_RUN_REPORT_MS 10000u  /* a line this often while it runs */

/* "haptic" with no arguments plays the alert's own pattern. */

#ifdef CONFIG_VS_HAPTIC_ALERT
#  define CLI_HAPTIC_PULSES CONFIG_VS_HAPTIC_ALERT_PULSES
#  define CLI_HAPTIC_ON_MS  CONFIG_VS_HAPTIC_ALERT_ON_MS
#  define CLI_HAPTIC_OFF_MS CONFIG_VS_HAPTIC_ALERT_OFF_MS
#else
#  define CLI_HAPTIC_PULSES 2
#  define CLI_HAPTIC_ON_MS  80
#  define CLI_HAPTIC_OFF_MS 120
#endif

/* Page sets, as masks over enum vs_page_e. */

#define CLI_PAGE(p)  (1ul << (p))
#define CLI_BROWSE   (CLI_PAGE(VS_PAGE_HISTORY) | \
                      CLI_PAGE(VS_PAGE_HISTORY_BLANK))
#define CLI_LIVE     (CLI_PAGE(VS_PAGE_SOCIAL_RUNNING) | \
                      CLI_PAGE(VS_PAGE_SOCIAL_ALERT))
#define CLI_SESSION  (CLI_LIVE | CLI_PAGE(VS_PAGE_SOCIAL_PAUSED) | \
                      CLI_PAGE(VS_PAGE_SOCIAL_PAUSING) | \
                      CLI_PAGE(VS_PAGE_SOCIAL_RESUMING) | \
                      CLI_PAGE(VS_PAGE_SOCIAL_EXITING))
#define CLI_VOICE    (CLI_PAGE(VS_PAGE_VOICE_LISTENING) | \
                      CLI_PAGE(VS_PAGE_VOICE_THINKING) | \
                      CLI_PAGE(VS_PAGE_VOICE_SPEAKING) | \
                      CLI_PAGE(VS_PAGE_PHOTO_CAPTURE))
#define CLI_ERROR    CLI_PAGE(VS_PAGE_ERROR)

#define CLI_LINE 640

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* What a command can do with one key, in the terms vs_input.c reports. */

enum cli_gesture_e
{
  CLI_SHORT = 0,  /* a tap */
  CLI_LONG,       /* the long-press event on its own */
  CLI_HOLD,       /* held to completion: progress, then long */
  CLI_CANCEL,     /* held, then let go early: progress, then cancel */
  CLI_COMBO       /* 返回 and 下一条 held together to completion */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Sized by the enums, so a page added there prints as "unknown" here rather
 * than as the next page's name.
 */

static const char *const g_cli_pages[VS_PAGE_ERROR + 1] =
{
  "preparing", "history", "history_blank", "volume", "saving",
  "social_enter", "social_starting", "social_running", "social_alert",
  "social_pausing", "social_paused", "social_resuming", "social_exiting",
  "social_finalizing", "social_result", "voice_listening",
  "voice_thinking", "voice_speaking", "photo_capture", "net_switching",
  "softap", "error"
};

static const char *const g_cli_stages[VS_SOCIAL_STAGE_COUNT] =
{
  "none", "stopping", "closing", "waiting", "saving", "fetching"
};

static const char *const g_cli_keys[VS_KEY_COUNT] =
{
  "confirm", "back", "next"
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void cli_cat(char *line, size_t size, size_t *len,
                    const char *format, ...) printf_like(4, 5);
static int cli_ok(const char *cmd, const struct vs_app_status_s *st,
                  const char *format, ...) printf_like(3, 4);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t cli_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static bool cli_expired(uint32_t deadline)
{
  return (int32_t)(cli_now_ms() - deadline) >= 0;
}

static const char *cli_page_name(enum vs_page_e page)
{
  if ((unsigned int)page > VS_PAGE_ERROR || g_cli_pages[page] == NULL)
    {
      return "unknown";
    }

  return g_cli_pages[page];
}

static const char *cli_stage_name(enum vs_social_stage_e stage)
{
  if ((unsigned int)stage >= VS_SOCIAL_STAGE_COUNT)
    {
      return "unknown";
    }

  return g_cli_stages[stage];
}

static const char *cli_net_name(const struct vs_app_status_s *st)
{
  switch (st->net_state)
    {
      case VS_NET_DOWN:
        return "down";

      case VS_NET_SWITCHING:
        return "switching";

      case VS_NET_STA_READY:
        return "sta_ready";

      case VS_NET_AP_READY:
        return "ap_ready";

      case VS_NET_ERROR:
        return "error";

      default:
        return "unknown";
    }
}

static int cli_find_page(const char *name)
{
  int page;

  for (page = 0; page <= VS_PAGE_ERROR; page++)
    {
      if (g_cli_pages[page] != NULL && strcmp(g_cli_pages[page], name) == 0)
        {
          return page;
        }
    }

  return -1;
}

/* Append to a line being built, truncating rather than overrunning. */

static void cli_cat(char *line, size_t size, size_t *len,
                    const char *format, ...)
{
  va_list ap;
  int n;

  if (*len + 1 >= size)
    {
      return;
    }

  va_start(ap, format);
  n = vsnprintf(line + *len, size - *len, format, ap);
  va_end(ap);

  if (n > 0)
    {
      *len = (size_t)n < size - *len ? *len + (size_t)n : size - 1;
    }
}

/* Append key="text" with the text kept on one line: a newline becomes \n and
 * a double quote a single one, so every field stays parseable.
 */

static void cli_cat_quoted(char *line, size_t size, size_t *len,
                           const char *key, const char *text)
{
  cli_cat(line, size, len, " %s=\"", key);

  for (; *text != '\0' && *len + 4 < size; text++)
    {
      char c = *text;

      if (c == '\n')
        {
          line[(*len)++] = '\\';
          line[(*len)++] = 'n';
        }
      else if (c == '"')
        {
          line[(*len)++] = '\'';
        }
      else if ((unsigned char)c < 0x20)
        {
          line[(*len)++] = ' ';
        }
      else
        {
          line[(*len)++] = c;
        }
    }

  line[*len] = '\0';
  cli_cat(line, size, len, "\"");
}

/* Read the status, treating a UI that is not up yet as no UI. */

static int cli_read(struct vs_app_status_s *st)
{
  int ret = vs_app_get_status(st);

  if (ret == 0 && !st->ready)
    {
      ret = -ENODEV;
    }

  return ret;
}

/****************************************************************************
 * Name: cli_print_status
 *
 * Description:
 *   Three lines: the state, then each screen as it is drawn now.
 *
 *     vs_cli: status page=... req=... stage=... err=... net=... ip=...
 *     vs_cli: left title="..." body="..." meta="..."
 *     vs_cli: right title="..." value="..." meta="..." confirm="..." ...
 *
 *   The key fields are the labels on screen, empty when a key is hidden,
 *   which is how a script tells what a page will accept.
 *
 ****************************************************************************/

static void cli_print_status(const struct vs_app_status_s *st)
{
  char line[CLI_LINE];
  size_t len = 0;
  int key;

  line[0] = '\0';
  cli_cat(line, sizeof(line), &len,
          "vs_cli: status page=%s req=%lu stage=%s err=%d net=%s ip=%s",
          cli_page_name(st->page), (unsigned long)st->request_id,
          cli_stage_name(st->social_stage), st->error, cli_net_name(st),
          st->net_address[0] != '\0' ? st->net_address : "-");
  cli_cat_quoted(line, sizeof(line), &len, "ssid", st->net_ssid);
  cli_cat(line, sizeof(line), &len,
          " vol=%u%s hist=%u/%u social=%d voice=%d tts=%d alert=%d"
          " inputs=%lu",
          (unsigned int)st->volume, st->volume_editing ? "(edit)" : "",
          st->history_blank ? 0u : (unsigned int)st->history_index + 1u,
          (unsigned int)st->history_count, vs_social_active(),
          vs_voice_ready(), vs_tts_busy(), st->alert_active,
          (unsigned long)st->inputs);

  if (st->page == VS_PAGE_ERROR)
    {
      cli_cat(line, sizeof(line), &len, " retry=%s",
              !st->error_retryable ? "none" :
              st->error_retry == 2 ? "social" : "network");
      cli_cat_quoted(line, sizeof(line), &len, "reason", st->error_reason);
    }

  printf("%s\n", line);

  len = 0;
  line[0] = '\0';
  cli_cat(line, sizeof(line), &len, "vs_cli: left");
  cli_cat_quoted(line, sizeof(line), &len, "title", st->content_title);
  cli_cat_quoted(line, sizeof(line), &len, "body", st->content_body);
  cli_cat_quoted(line, sizeof(line), &len, "meta", st->content_meta);
  printf("%s\n", line);

  len = 0;
  line[0] = '\0';
  cli_cat(line, sizeof(line), &len, "vs_cli: right");
  cli_cat_quoted(line, sizeof(line), &len, "title", st->status_title);
  cli_cat_quoted(line, sizeof(line), &len, "value", st->status_value);
  cli_cat_quoted(line, sizeof(line), &len, "meta", st->status_meta);
  for (key = 0; key < VS_KEY_COUNT; key++)
    {
      cli_cat_quoted(line, sizeof(line), &len, g_cli_keys[key],
                     st->softkey[key]);
    }

  printf("%s\n", line);
}

/* The closing line of a command.  st may be NULL when there is no UI. */

static int cli_finish(bool ok, const char *cmd,
                      const struct vs_app_status_s *st, const char *why,
                      const char *detail)
{
  char line[CLI_LINE];
  size_t len = 0;

  line[0] = '\0';
  cli_cat(line, sizeof(line), &len, "vs_cli: %s %s page=%s",
          ok ? "ok" : "fail", cmd,
          st != NULL ? cli_page_name(st->page) : "none");

  if (!ok)
    {
      cli_cat_quoted(line, sizeof(line), &len, "why",
                     why != NULL ? why : "failed");
    }

  if (detail != NULL && detail[0] != '\0')
    {
      cli_cat(line, sizeof(line), &len, " %s", detail);
    }

  if (!ok && st != NULL && st->page == VS_PAGE_ERROR)
    {
      cli_cat_quoted(line, sizeof(line), &len, "reason", st->error_reason);
    }

  printf("%s\n", line);
  return ok ? 0 : 1;
}

static int cli_ok(const char *cmd, const struct vs_app_status_s *st,
                  const char *format, ...)
{
  char detail[CLI_LINE / 2];
  va_list ap;

  detail[0] = '\0';
  if (format != NULL)
    {
      va_start(ap, format);
      vsnprintf(detail, sizeof(detail), format, ap);
      va_end(ap);
    }

  return cli_finish(true, cmd, st, NULL, detail);
}

/* cli_ok() with nothing to add. */

static int cli_done(const char *cmd, const struct vs_app_status_s *st)
{
  return cli_finish(true, cmd, st, NULL, NULL);
}

static int cli_fail(const char *cmd, const struct vs_app_status_s *st,
                    const char *why)
{
  return cli_finish(false, cmd, st, why, NULL);
}

/* Queue one event as the key task would have. */

static int cli_push(enum vs_input_event_e type, enum vs_key_e key,
                    uint8_t progress, uint32_t held_ms)
{
  struct vs_input_event_s event;

  memset(&event, 0, sizeof(event));
  event.type     = type;
  event.key      = key;
  event.progress = progress;
  event.held_ms  = held_ms;
  return vs_app_inject_input(&event);
}

/****************************************************************************
 * Name: cli_gesture
 *
 * Description:
 *   Inject one gesture and wait until the UI has handled every event in it,
 *   leaving *st as the status that followed.
 *
 *   A gesture is the events vs_input.c reports for it, without the run of
 *   intermediate progress values: no handler needs them, and a hold the UI
 *   acts on is the one that reached the long press.  The wait is on the
 *   UI's handled-input count rather than on a page, because some gestures
 *   legitimately leave the page where it was.
 *
 ****************************************************************************/

static int cli_gesture(enum vs_key_e key, enum cli_gesture_e gesture,
                       struct vs_app_status_s *st)
{
  uint32_t before;
  uint32_t deadline;
  uint32_t count;
  int ret;

  ret = cli_read(st);
  if (ret < 0)
    {
      return ret;
    }

  before = st->inputs;
  count  = 1;

  switch (gesture)
    {
      case CLI_SHORT:
        ret = cli_push(VS_INPUT_SHORT, key, 0, 120);
        break;

      case CLI_LONG:
        ret = cli_push(VS_INPUT_LONG, key, 100, CONFIG_VS_LONG_PRESS_MS);
        break;

      case CLI_HOLD:
        count = 2;
        ret = cli_push(VS_INPUT_PROGRESS, key, 50,
                       CONFIG_VS_LONG_PRESS_MS / 2);
        if (ret == 0)
          {
            ret = cli_push(VS_INPUT_LONG, key, 100,
                           CONFIG_VS_LONG_PRESS_MS);
          }
        break;

      case CLI_CANCEL:
        count = 2;
        ret = cli_push(VS_INPUT_PROGRESS, key, 50,
                       CONFIG_VS_LONG_PRESS_MS / 2);
        if (ret == 0)
          {
            ret = cli_push(VS_INPUT_CANCEL, key, 0,
                           CONFIG_VS_LONG_PRESS_MS / 2);
          }
        break;

      case CLI_COMBO:
        count = 2;
        ret = cli_push(VS_INPUT_COMBO_PROGRESS, VS_KEY_BACK, 0, 0);
        if (ret == 0)
          {
            ret = cli_push(VS_INPUT_NET_TOGGLE, VS_KEY_BACK, 100,
                           CONFIG_VS_COMBO_PRESS_MS);
          }
        break;

      default:
        return -EINVAL;
    }

  if (ret < 0)
    {
      return ret;
    }

  deadline = cli_now_ms() + CLI_INPUT_MS;
  for (; ; )
    {
      ret = cli_read(st);
      if (ret < 0)
        {
          return ret;
        }

      if (st->inputs - before >= count)
        {
          return 0;
        }

      if (cli_expired(deadline))
        {
          return -ETIMEDOUT;
        }

      usleep(20 * 1000);
    }
}

/****************************************************************************
 * Name: cli_wait
 *
 * Description:
 *   Wait until the page is in want (0), in fail (-EIO), or timeout_ms has
 *   passed (-ETIMEDOUT), leaving *st as the last status read.  A line is
 *   printed whenever the page or the finalize stage moves, so a long wait
 *   leaves a trail of what it went through -- and on the finalizing page
 *   whenever its line changes too, detail="...", so the trail also shows
 *   the uploads counting down, the seconds waited and the audio arriving.
 *
 ****************************************************************************/

static int cli_wait(unsigned long want, unsigned long fail,
                    uint32_t timeout_ms, struct vs_app_status_s *st)
{
  uint32_t deadline = cli_now_ms() + timeout_ms;
  char last_body[VS_TEXT_LONG];
  int last_page = -1;
  int last_stage = -1;
  int ret;

  last_body[0] = '\0';

  for (; ; )
    {
      bool finalizing;

      ret = cli_read(st);
      if (ret < 0)
        {
          return ret;
        }

      finalizing = st->page == VS_PAGE_SOCIAL_FINALIZING;

      if ((int)st->page != last_page ||
          (int)st->social_stage != last_stage ||
          (finalizing && strcmp(st->content_body, last_body) != 0))
        {
          char line[CLI_LINE / 2];
          size_t len = 0;

          last_page  = (int)st->page;
          last_stage = (int)st->social_stage;
          snprintf(last_body, sizeof(last_body), "%s", st->content_body);

          line[0] = '\0';
          cli_cat(line, sizeof(line), &len, "vs_cli: page %s stage %s",
                  cli_page_name(st->page), cli_stage_name(st->social_stage));
          if (finalizing)
            {
              cli_cat_quoted(line, sizeof(line), &len, "detail",
                             st->content_body);
            }

          printf("%s\n", line);
        }

      if ((CLI_PAGE(st->page) & want) != 0)
        {
          return 0;
        }

      if ((CLI_PAGE(st->page) & fail) != 0)
        {
          return -EIO;
        }

      if (cli_expired(deadline))
        {
          return -ETIMEDOUT;
        }

      usleep(CLI_POLL_MS * 1000);
    }
}

/****************************************************************************
 * Name: cli_home
 *
 * Description:
 *   Bring the UI to a browse page -- the history or the blank home -- from
 *   wherever a command may have left it, without ending anything that is
 *   still running.  -EBUSY when a session, a voice round or the hotspot is
 *   in the way: ending one of those is a command of its own, not a side
 *   effect of another.
 *
 ****************************************************************************/

static int cli_home(struct vs_app_status_s *st)
{
  unsigned int step;
  int ret;

  for (step = 0; step < 6; step++)
    {
      ret = cli_read(st);
      if (ret < 0)
        {
          return ret;
        }

      if ((CLI_PAGE(st->page) & CLI_BROWSE) != 0)
        {
          return 0;
        }

      switch (st->page)
        {
          case VS_PAGE_SOCIAL_RESULT:
          case VS_PAGE_ERROR:
            ret = cli_gesture(VS_KEY_BACK, CLI_SHORT, st);
            break;

          case VS_PAGE_VOLUME:

            /* Confirm while editing, which saves the level; next otherwise,
             * which is the ring's way round to the blank home.
             */

            ret = cli_gesture(st->volume_editing ? VS_KEY_CONFIRM :
                                                   VS_KEY_NEXT,
                              CLI_SHORT, st);
            break;

          case VS_PAGE_SOCIAL_ENTER:
            ret = cli_gesture(VS_KEY_CONFIRM, CLI_CANCEL, st);
            break;

          case VS_PAGE_SOCIAL_EXITING:
            ret = cli_gesture(VS_KEY_BACK, CLI_CANCEL, st);
            break;

          case VS_PAGE_SAVING:
          case VS_PAGE_PREPARING:
            ret = cli_wait(~CLI_PAGE(st->page), 0, CLI_SETTLE_MS, st);
            break;

          default:
            return -EBUSY;
        }

      if (ret < 0)
        {
          return ret;
        }
    }

  return -EAGAIN;
}

static const char *cli_home_why(int ret)
{
  return ret == -EBUSY ? "a session, voice round or hotspot is in the way" :
                         "could not get back to a browse page";
}

/* Leave a page the UI only passes through, so the next step sees where it
 * settled.
 */

static int cli_settle(struct vs_app_status_s *st)
{
  if (st->page == VS_PAGE_SAVING)
    {
      return cli_wait(~CLI_PAGE(VS_PAGE_SAVING), 0, CLI_SETTLE_MS, st);
    }

  return 0;
}

/****************************************************************************
 * The social session
 ****************************************************************************/

/* Start a session from a browse page and wait until it is sampling.  On
 * failure *why says what went wrong.
 */

static int cli_social_start(struct vs_app_status_s *st, const char **why)
{
  int ret;

  ret = cli_read(st);
  if (ret < 0)
    {
      *why = "the UI is not running";
      return ret;
    }

  if ((CLI_PAGE(st->page) & CLI_LIVE) != 0)
    {
      return 0;
    }

  ret = cli_home(st);
  if (ret < 0)
    {
      *why = cli_home_why(ret);
      return ret;
    }

  if (st->net_state != VS_NET_STA_READY)
    {
      printf("vs_cli: note network is %s; the cloud session may not open\n",
             cli_net_name(st));
    }

  /* Holding 确认 on a browse page: the hold opens the entry page and the
   * long press starts the session from it, as on the board.
   */

  ret = cli_gesture(VS_KEY_CONFIRM, CLI_HOLD, st);
  if (ret < 0)
    {
      *why = "the UI did not take the input";
      return ret;
    }

  ret = cli_wait(CLI_LIVE, CLI_ERROR | CLI_BROWSE, CLI_START_MS, st);
  if (ret < 0)
    {
      *why = ret == -ETIMEDOUT ? "the session did not answer" :
                                 "the session did not start";
    }

  return ret;
}

/* End the session and wait for its minutes. */

static int cli_social_stop(struct vs_app_status_s *st, const char **why)
{
  int ret;

  ret = cli_read(st);
  if (ret < 0)
    {
      *why = "the UI is not running";
      return ret;
    }

  /* The pausing and resuming pages take no exit gesture.  Neither lasts
   * longer than a round trip, so wait for the one they settle on.
   */

  if (st->page == VS_PAGE_SOCIAL_PAUSING ||
      st->page == VS_PAGE_SOCIAL_RESUMING)
    {
      ret = cli_wait(CLI_LIVE | CLI_PAGE(VS_PAGE_SOCIAL_PAUSED), CLI_ERROR,
                     CLI_PAUSE_MS, st);
      if (ret < 0)
        {
          *why = "the session did not settle";
          return ret;
        }
    }

  if ((CLI_PAGE(st->page) & CLI_SESSION) == 0)
    {
      *why = st->page == VS_PAGE_SOCIAL_STARTING ?
             "the session is still starting" : "no session is running";
      return -EINVAL;
    }

  /* Holding 返回: the hold opens the exit page, the long press ends it. */

  ret = cli_gesture(VS_KEY_BACK, CLI_HOLD, st);
  if (ret < 0)
    {
      *why = "the UI did not take the input";
      return ret;
    }

  ret = cli_wait(CLI_PAGE(VS_PAGE_SOCIAL_RESULT), CLI_ERROR | CLI_BROWSE,
                 CLI_FINALIZE_MS, st);
  if (ret < 0)
    {
      *why = ret == -ETIMEDOUT ? "no minutes and no error from the UI" :
                                 "the session ended without minutes";
    }

  return ret;
}

/* Run a whole session: start, sample for seconds, stop, wait for minutes. */

static int cli_social_run(unsigned int seconds, struct vs_app_status_s *st,
                          const char **why)
{
  uint32_t began;
  uint32_t until;
  uint32_t report;
  int ret;

  ret = cli_social_start(st, why);
  if (ret < 0)
    {
      return ret;
    }

  printf("vs_cli: session running req=%lu, stopping in %u s\n",
         (unsigned long)st->request_id, seconds);

  began  = cli_now_ms();
  until  = began + seconds * 1000u;
  report = began + CLI_RUN_REPORT_MS;

  while (!cli_expired(until))
    {
      usleep(5 * CLI_POLL_MS * 1000);

      ret = cli_read(st);
      if (ret < 0)
        {
          *why = "the UI stopped";
          return ret;
        }

      if ((CLI_PAGE(st->page) & CLI_SESSION) == 0)
        {
          *why = "the session ended before it was stopped";
          return -EIO;
        }

      if (cli_expired(report))
        {
          printf("vs_cli: running t=%lus page=%s alert=%d\n",
                 (unsigned long)((cli_now_ms() - began) / 1000u),
                 cli_page_name(st->page), st->alert_active);
          report += CLI_RUN_REPORT_MS;
        }
    }

  return cli_social_stop(st, why);
}

/* The minutes, as the result page shows them. */

static void cli_print_result(const struct vs_app_status_s *st)
{
  char line[CLI_LINE];
  size_t len = 0;

  line[0] = '\0';
  cli_cat(line, sizeof(line), &len, "vs_cli: result");
  cli_cat_quoted(line, sizeof(line), &len, "summary", st->content_body);
  printf("%s\n", line);
}

/* "social tune [default] [key=value ...]": show or change the pacing the
 * next session runs to.  Keys are interval, max, backlog, fps and poll; see
 * vs_social_tune_s.  "default" puts back the Kconfig values before any key
 * after it applies.  A max below the interval is raised to it unless max was
 * given, so "interval=3000" alone does the obvious thing.
 */

static int cli_social_tune(int argc, char *argv[],
                           struct vs_app_status_s *st)
{
  struct vs_social_tune_s tune;
  bool max_given = false;
  int i;
  int ret;

  vs_social_get_tune(&tune);

  for (i = 3; i < argc; i++)
    {
      const char *arg = argv[i];
      const char *eq = strchr(arg, '=');
      unsigned long value;

      if (strcmp(arg, "default") == 0)
        {
          vs_social_default_tune(&tune);
          continue;
        }

      if (eq == NULL || !isdigit((unsigned char)eq[1]))
        {
          printf("vs_cli: tune takes key=value with interval, max, "
                 "backlog, fps or poll\n");
          return 2;
        }

      value = strtoul(eq + 1, NULL, 10);
      if (strncmp(arg, "interval=", 9) == 0)
        {
          tune.image_interval_ms = (uint32_t)value;
        }
      else if (strncmp(arg, "max=", 4) == 0)
        {
          tune.image_interval_max_ms = (uint32_t)value;
          max_given = true;
        }
      else if (strncmp(arg, "backlog=", 8) == 0)
        {
          tune.backlog_max = (uint32_t)value;
        }
      else if (strncmp(arg, "fps=", 4) == 0)
        {
          tune.capture_fps = (uint32_t)value;
        }
      else if (strncmp(arg, "poll=", 5) == 0)
        {
          tune.poll_interval_ms = (uint32_t)value;
        }
      else
        {
          printf("vs_cli: unknown tune key \"%.*s\"\n", (int)(eq - arg),
                 arg);
          return 2;
        }
    }

  if (!max_given && tune.image_interval_max_ms < tune.image_interval_ms)
    {
      tune.image_interval_max_ms = tune.image_interval_ms;
    }

  if (argc > 3)
    {
      ret = vs_social_set_tune(&tune);
      if (ret < 0)
        {
          return cli_fail("social tune", st, ret == -EBUSY ?
                          "a session is in flight" :
                          "a value is out of range");
        }

      vs_social_get_tune(&tune);
    }

  return cli_ok("social tune", st,
                "interval=%lu max=%lu backlog=%lu fps=%lu poll=%lu",
                (unsigned long)tune.image_interval_ms,
                (unsigned long)tune.image_interval_max_ms,
                (unsigned long)tune.backlog_max,
                (unsigned long)tune.capture_fps,
                (unsigned long)tune.poll_interval_ms);
}

/****************************************************************************
 * Name: cli_social
 *
 * Description:
 *   "velasight social <start|stop|pause|resume|cancel|retry|run [s]>", and
 *   the old "velasight social [seconds]" when a UI is running -- which now
 *   means a whole session driven through it.  That form also prints the
 *   headless probe's last line, so a script matching on it keeps working.
 *
 ****************************************************************************/

static int cli_social(int argc, char *argv[], struct vs_app_status_s *st)
{
  const char *sub = argc > 2 ? argv[2] : "run";
  const char *why = NULL;
  bool legacy = argc <= 2 || isdigit((unsigned char)argv[2][0]);
  char cmd[32];
  int ret;

  snprintf(cmd, sizeof(cmd), "social %s", legacy ? "run" : sub);

  if (legacy || strcmp(sub, "run") == 0)
    {
      const char *arg = legacy ? (argc > 2 ? argv[2] : NULL) :
                                 (argc > 3 ? argv[3] : NULL);

      /* The bare legacy form keeps the headless probe's 15 s default. */

      int seconds = arg != NULL ? atoi(arg) :
                    legacy ? 15 : (int)CLI_RUN_SECONDS;

      if (seconds < 1 || seconds > 600)
        {
          printf("vs_cli: social run takes 1..600 seconds\n");
          return 2;
        }

      ret = cli_social_run((unsigned int)seconds, st, &why);
      if (ret == 0)
        {
          cli_print_result(st);
        }

      if (legacy)
        {
          printf("velasight: social probe %s\n", ret == 0 ? "completed" :
                 "did not produce minutes");
        }

      return ret == 0 ? cli_ok(cmd, st, "seconds=%d", seconds) :
                        cli_fail(cmd, st, why);
    }

  if (strcmp(sub, "tune") == 0)
    {
      return cli_social_tune(argc, argv, st);
    }

  if (strcmp(sub, "start") == 0)
    {
      ret = cli_social_start(st, &why);
      return ret == 0 ? cli_ok(cmd, st, "req=%lu",
                               (unsigned long)st->request_id) :
                        cli_fail(cmd, st, why);
    }

  if (strcmp(sub, "stop") == 0)
    {
      ret = cli_social_stop(st, &why);
      if (ret == 0)
        {
          cli_print_result(st);
        }

      return ret == 0 ? cli_done(cmd, st) : cli_fail(cmd, st, why);
    }

  if (strcmp(sub, "pause") == 0 || strcmp(sub, "resume") == 0)
    {
      bool pause = sub[0] == 'p';
      unsigned long from = pause ? CLI_LIVE :
                           CLI_PAGE(VS_PAGE_SOCIAL_PAUSED);
      unsigned long to = pause ? CLI_PAGE(VS_PAGE_SOCIAL_PAUSED) : CLI_LIVE;

      if ((CLI_PAGE(st->page) & to) != 0)
        {
          return cli_ok(cmd, st, "already=1");
        }

      if ((CLI_PAGE(st->page) & from) == 0)
        {
          return cli_fail(cmd, st, pause ? "no running session to pause" :
                                           "the session is not paused");
        }

      ret = cli_gesture(VS_KEY_CONFIRM, CLI_SHORT, st);
      if (ret == 0)
        {
          ret = cli_wait(to, CLI_ERROR | CLI_BROWSE, CLI_PAUSE_MS, st);
        }

      return ret == 0 ? cli_done(cmd, st) :
                        cli_fail(cmd, st, "the session did not follow");
    }

  if (strcmp(sub, "cancel") == 0)
    {
      if (st->page != VS_PAGE_SOCIAL_STARTING)
        {
          return cli_fail(cmd, st, "only a session still starting can be "
                                   "cancelled; use social stop");
        }

      ret = cli_gesture(VS_KEY_BACK, CLI_SHORT, st);
      if (ret == 0)
        {
          ret = cli_wait(CLI_BROWSE, CLI_ERROR, CLI_SETTLE_MS, st);
        }

      return ret == 0 ? cli_done(cmd, st) :
                        cli_fail(cmd, st, "the session did not cancel");
    }

  if (strcmp(sub, "retry") == 0)
    {
      if (st->page != VS_PAGE_ERROR || !st->error_retryable ||
          st->error_retry != 2)
        {
          return cli_fail(cmd, st, "no social retry is offered");
        }

      ret = cli_gesture(VS_KEY_CONFIRM, CLI_SHORT, st);
      if (ret == 0 && st->page == VS_PAGE_ERROR)
        {
          /* Still on the error page: the previous attempt is unwinding and
           * the key only flashed 稍等, or the retry was refused outright.
           */

          return cli_fail(cmd, st, "the retry was not accepted yet");
        }

      if (ret == 0)
        {
          ret = cli_wait(CLI_PAGE(VS_PAGE_SOCIAL_RESULT),
                         CLI_ERROR | CLI_BROWSE, CLI_FINALIZE_MS, st);
        }

      if (ret == 0)
        {
          cli_print_result(st);
        }

      return ret == 0 ? cli_done(cmd, st) :
                        cli_fail(cmd, st, "no minutes from the retry");
    }

  printf("vs_cli: unknown social command \"%s\"\n", sub);
  return 2;
}

/****************************************************************************
 * The voice assistant
 ****************************************************************************/

/* Wait for the microphone to be open, which is when the listening page
 * starts asking the user to speak.
 */

static int cli_wait_listening(struct vs_app_status_s *st, const char **why)
{
  uint32_t deadline = cli_now_ms() + CLI_VOICE_MS;
  int ret;

  for (; ; )
    {
      ret = cli_read(st);
      if (ret < 0)
        {
          *why = "the UI is not running";
          return ret;
        }

      if (st->page == VS_PAGE_VOICE_LISTENING && !st->voice_arming)
        {
          return 0;
        }

      if ((CLI_PAGE(st->page) & (CLI_ERROR | CLI_BROWSE)) != 0)
        {
          *why = "the voice round ended before it listened";
          return -EIO;
        }

      if (cli_expired(deadline))
        {
          *why = "the microphone did not open";
          return -ETIMEDOUT;
        }

      usleep(CLI_POLL_MS * 1000);
    }
}

/* "ask": a voice question about the record on screen.  "photo": a photo
 * question from the blank home page.
 */

static int cli_voice_begin(bool photo, struct vs_app_status_s *st,
                           const char **why)
{
  unsigned int step;
  int ret;

  ret = cli_home(st);
  if (ret < 0)
    {
      *why = cli_home_why(ret);
      return ret;
    }

  if (photo)
    {
      /* Back down the records to the blank page, where 拍照提问 is. */

      for (step = 0; st->page == VS_PAGE_HISTORY &&
                     step <= st->history_count; step++)
        {
          ret = cli_gesture(VS_KEY_BACK, CLI_SHORT, st);
          if (ret < 0)
            {
              break;
            }
        }
    }
  else if (st->page == VS_PAGE_HISTORY_BLANK)
    {
      if (st->history_count == 0)
        {
          *why = "there is no record to ask about";
          return -ENOENT;
        }

      ret = cli_gesture(VS_KEY_NEXT, CLI_SHORT, st);
    }

  if (ret < 0)
    {
      *why = "the UI did not take the input";
      return ret;
    }

  if (st->page != (photo ? VS_PAGE_HISTORY_BLANK : VS_PAGE_HISTORY))
    {
      *why = photo ? "could not reach the blank home page" :
                     "could not reach a record";
      return -EINVAL;
    }

  ret = cli_gesture(VS_KEY_CONFIRM, CLI_SHORT, st);
  if (ret < 0)
    {
      *why = "the UI did not take the input";
      return ret;
    }

  if ((CLI_PAGE(st->page) & CLI_VOICE) == 0)
    {
      *why = vs_voice_ready() ? "the voice round did not start" :
                                "the voice assistant is not ready yet";
      return -EAGAIN;
    }

  return cli_wait_listening(st, why);
}

static int cli_voice(int argc, char *argv[], struct vs_app_status_s *st)
{
  const char *sub = argv[1];
  const char *why = NULL;
  char cmd[32];
  int ret;

  if (strcmp(sub, "voice") == 0)
    {
      if (argc < 3)
        {
          printf("vs_cli: voice takes done or exit\n");
          return 2;
        }

      sub = argv[2];
    }

  snprintf(cmd, sizeof(cmd), "%s%s", strcmp(argv[1], "voice") == 0 ?
           "voice " : "", sub);

  if (strcmp(sub, "ask") == 0 || strcmp(sub, "photo") == 0)
    {
      ret = cli_voice_begin(sub[0] == 'p', st, &why);
      return ret == 0 ? cli_ok(cmd, st, "listening=1") :
                        cli_fail(cmd, st, why);
    }

  if (strcmp(sub, "done") == 0)
    {
      if (st->page != VS_PAGE_VOICE_LISTENING || st->voice_arming)
        {
          return cli_fail(cmd, st, "not listening");
        }

      /* 说完 ends this utterance only; the round goes on to the answer. */

      ret = cli_gesture(VS_KEY_CONFIRM, CLI_SHORT, st);
      if (ret == 0)
        {
          ret = cli_wait(~CLI_PAGE(VS_PAGE_VOICE_LISTENING), 0,
                         CLI_VOICE_MS, st);
        }

      return ret == 0 ? cli_done(cmd, st) :
                        cli_fail(cmd, st, "the round did not move on");
    }

  if (strcmp(sub, "exit") == 0)
    {
      if ((CLI_PAGE(st->page) & CLI_VOICE) == 0)
        {
          return cli_fail(cmd, st, "no voice round is open");
        }

      ret = cli_gesture(VS_KEY_BACK, CLI_SHORT, st);
      if (ret == 0)
        {
          ret = cli_wait(CLI_BROWSE, CLI_ERROR, CLI_VOICE_MS, st);
        }

      return ret == 0 ? cli_done(cmd, st) :
                        cli_fail(cmd, st, "the round did not end");
    }

  printf("vs_cli: unknown voice command \"%s\"\n", sub);
  return 2;
}

/****************************************************************************
 * Browsing, volume and network
 ****************************************************************************/

static int cli_browse(bool forward, struct vs_app_status_s *st)
{
  const char *cmd = forward ? "next" : "prev";
  int ret;

  if ((CLI_PAGE(st->page) & (CLI_BROWSE | CLI_PAGE(VS_PAGE_VOLUME))) == 0 ||
      st->volume_editing)
    {
      return cli_fail(cmd, st, "not on a browse page");
    }

  ret = cli_gesture(forward ? VS_KEY_NEXT : VS_KEY_BACK, CLI_SHORT, st);
  if (ret == 0)
    {
      ret = cli_settle(st);
    }

  if (ret < 0)
    {
      return cli_fail(cmd, st, "the UI did not take the input");
    }

  return cli_ok(cmd, st, "hist=%u/%u",
                st->history_blank ? 0u : (unsigned int)st->history_index + 1u,
                (unsigned int)st->history_count);
}

/****************************************************************************
 * Name: cli_volume
 *
 * Description:
 *   "volume up|down|<0..100>".  Goes to the volume page, opens it for
 *   editing, steps the level with the rocker keys and confirms, which is
 *   what saves it -- the same sequence a person goes through, five percent
 *   a press.
 *
 ****************************************************************************/

static int cli_volume(int argc, char *argv[], struct vs_app_status_s *st)
{
  const char *cmd = "volume";
  unsigned int step;
  int target;
  int ret = 0;

  if (argc < 3)
    {
      printf("vs_cli: volume takes up, down or 0..100\n");
      return 2;
    }

  if (st->page != VS_PAGE_VOLUME)
    {
      ret = cli_home(st);
      if (ret < 0)
        {
          return cli_fail(cmd, st, cli_home_why(ret));
        }

      /* The ring runs blank, records, volume: back from the blank page, or
       * forwards off the last record.
       */

      if (st->page == VS_PAGE_HISTORY_BLANK)
        {
          ret = cli_gesture(VS_KEY_BACK, CLI_SHORT, st);
        }

      for (step = 0; ret == 0 && st->page == VS_PAGE_HISTORY &&
                     step <= st->history_count; step++)
        {
          ret = cli_gesture(VS_KEY_NEXT, CLI_SHORT, st);
        }

      if (ret == 0)
        {
          ret = cli_settle(st);
        }

      if (ret < 0 || st->page != VS_PAGE_VOLUME)
        {
          return cli_fail(cmd, st, "could not reach the volume page");
        }
    }

  if (strcmp(argv[2], "up") == 0)
    {
      target = st->volume + 5;
    }
  else if (strcmp(argv[2], "down") == 0)
    {
      target = st->volume - 5;
    }
  else if (isdigit((unsigned char)argv[2][0]))
    {
      target = atoi(argv[2]);
    }
  else
    {
      printf("vs_cli: volume takes up, down or 0..100\n");
      return 2;
    }

  target = target < 0 ? 0 : target > 100 ? 100 : target;

  if (!st->volume_editing)
    {
      ret = cli_gesture(VS_KEY_CONFIRM, CLI_SHORT, st);
      if (ret < 0 || !st->volume_editing)
        {
          return cli_fail(cmd, st, "could not open the level for editing");
        }
    }

  /* Steps read back from the driver each time, so the walk follows where
   * the level actually is.  Bounded, in case a step ever stops moving it.
   */

  for (step = 0; step < 24; step++)
    {
      int now = st->volume;

      if (now + 2 >= target && now - 2 <= target)
        {
          break;
        }

      ret = cli_gesture(now < target ? VS_KEY_NEXT : VS_KEY_BACK,
                        CLI_SHORT, st);
      if (ret < 0)
        {
          break;
        }
    }

  /* Confirm closes the editing and saves the level. */

  if (ret == 0)
    {
      ret = cli_gesture(VS_KEY_CONFIRM, CLI_SHORT, st);
    }

  if (ret == 0)
    {
      ret = cli_settle(st);
    }

  if (ret < 0 || st->volume_editing)
    {
      return cli_fail(cmd, st, "the level was not saved");
    }

  return cli_ok(cmd, st, "volume=%u target=%d", (unsigned int)st->volume,
                target);
}

/* "net ap" opens the hotspot, "net sta" goes back to the configured Wi-Fi.
 * In AP mode the board leaves that network, so nothing that needs the cloud
 * works until "net sta".
 */

static int cli_net(int argc, char *argv[], struct vs_app_status_s *st)
{
  bool ap;
  char cmd[16];
  int ret;

  if (argc < 3 ||
      (strcmp(argv[2], "ap") != 0 && strcmp(argv[2], "sta") != 0))
    {
      printf("vs_cli: net takes ap or sta\n");
      return 2;
    }

  ap = argv[2][0] == 'a';
  snprintf(cmd, sizeof(cmd), "net %s", argv[2]);

  if (ap)
    {
      if (st->page == VS_PAGE_SOFTAP)
        {
          return cli_ok(cmd, st, "already=1");
        }

      ret = cli_home(st);
      if (ret < 0)
        {
          return cli_fail(cmd, st, cli_home_why(ret));
        }

      /* 返回 and 下一条 held together. */

      ret = cli_gesture(VS_KEY_BACK, CLI_COMBO, st);
      if (ret == 0)
        {
          ret = cli_wait(CLI_PAGE(VS_PAGE_SOFTAP), CLI_ERROR | CLI_BROWSE,
                         CLI_NET_MS, st);
        }
    }
  else
    {
      if ((CLI_PAGE(st->page) & CLI_BROWSE) != 0 &&
          st->net_mode == VS_NET_STA)
        {
          return cli_ok(cmd, st, "already=1");
        }

      if (st->page != VS_PAGE_SOFTAP)
        {
          return cli_fail(cmd, st, "not on the hotspot page");
        }

      /* Holding 返回 on the hotspot page. */

      ret = cli_gesture(VS_KEY_BACK, CLI_HOLD, st);
      if (ret == 0)
        {
          ret = cli_wait(CLI_BROWSE, CLI_ERROR, CLI_NET_MS, st);
        }
    }

  return ret == 0 ? cli_ok(cmd, st, "ip=%s",
                           st->net_address[0] != '\0' ?
                           st->net_address : "-") :
                    cli_fail(cmd, st, "the network did not switch");
}

/****************************************************************************
 * Raw gestures and waits
 ****************************************************************************/

/* "key <confirm|back|next|combo> [short|long|hold|cancel]" */

static int cli_key(int argc, char *argv[], struct vs_app_status_s *st)
{
  static const char *const gestures[] =
  {
    "short", "long", "hold", "cancel"
  };

  enum cli_gesture_e gesture = CLI_SHORT;
  enum vs_key_e key = VS_KEY_CONFIRM;
  bool combo = false;
  int i;
  int ret;

  if (argc < 3)
    {
      printf("vs_cli: key takes confirm, back, next or combo\n");
      return 2;
    }

  for (i = 0; i < VS_KEY_COUNT; i++)
    {
      if (strcmp(argv[2], g_cli_keys[i]) == 0)
        {
          break;
        }
    }

  if (strcmp(argv[2], "combo") == 0)
    {
      combo   = true;
      gesture = CLI_COMBO;
    }
  else if (i == VS_KEY_COUNT)
    {
      printf("vs_cli: unknown key \"%s\"\n", argv[2]);
      return 2;
    }
  else
    {
      key = (enum vs_key_e)i;
    }

  if (!combo && argc > 3)
    {
      for (i = 0; i < (int)(sizeof(gestures) / sizeof(gestures[0])); i++)
        {
          if (strcmp(argv[3], gestures[i]) == 0)
            {
              break;
            }
        }

      if (i == (int)(sizeof(gestures) / sizeof(gestures[0])))
        {
          printf("vs_cli: unknown gesture \"%s\"\n", argv[3]);
          return 2;
        }

      gesture = (enum cli_gesture_e)i;
    }

  ret = cli_gesture(key, gesture, st);
  if (ret < 0)
    {
      return cli_fail("key", st, "the UI did not take the input");
    }

  cli_print_status(st);
  return cli_ok("key", st, "%s=%s", combo ? "combo" : g_cli_keys[key],
                combo ? "hold" : gestures[gesture]);
}

/* "wait <page>[,<page>...] [seconds]" */

static int cli_wait_cmd(int argc, char *argv[], struct vs_app_status_s *st)
{
  unsigned long want = 0;
  char names[96];
  char *save = NULL;
  char *name;
  int seconds = 30;
  int ret;

  if (argc < 3)
    {
      printf("vs_cli: wait takes a page name\n");
      return 2;
    }

  snprintf(names, sizeof(names), "%s", argv[2]);
  for (name = strtok_r(names, ",", &save); name != NULL;
       name = strtok_r(NULL, ",", &save))
    {
      int page = cli_find_page(name);

      if (page < 0)
        {
          printf("vs_cli: unknown page \"%s\"\n", name);
          return 2;
        }

      want |= CLI_PAGE(page);
    }

  if (argc > 3)
    {
      seconds = atoi(argv[3]);
      if (seconds < 1 || seconds > 900)
        {
          printf("vs_cli: wait takes 1..900 seconds\n");
          return 2;
        }
    }

  ret = cli_wait(want, 0, (uint32_t)seconds * 1000u, st);
  return ret == 0 ? cli_done("wait", st) :
                    cli_fail("wait", st, "the page did not come up");
}

/****************************************************************************
 * Hardware, with or without a UI
 ****************************************************************************/

/****************************************************************************
 * Name: cli_haptic
 *
 * Description:
 *   "haptic [count [on_ms [off_ms]]]".  Vibrate now, with the emotion
 *   alert's pattern unless told otherwise, and end once the motor has
 *   stopped.  Whether it was felt is for the person holding the board:
 *   the PWM driver cannot tell a spinning motor from an unconnected pad.
 *
 ****************************************************************************/

static int cli_haptic(int argc, char *argv[],
                      const struct vs_app_status_s *st)
{
  unsigned int count  = CLI_HAPTIC_PULSES;
  unsigned int on_ms  = CLI_HAPTIC_ON_MS;
  unsigned int off_ms = CLI_HAPTIC_OFF_MS;
  uint32_t deadline;
  char detail[48];
  int ret;

  if (argc > 2)
    {
      count = (unsigned int)atoi(argv[2]);
    }

  if (argc > 3)
    {
      on_ms = (unsigned int)atoi(argv[3]);
    }

  if (argc > 4)
    {
      off_ms = (unsigned int)atoi(argv[4]);
    }

  ret = vs_haptic_pulse(count, on_ms, off_ms);
  if (ret < 0)
    {
      snprintf(detail, sizeof(detail), "err=%d", ret);
      return cli_finish(false, "haptic", st,
                        ret == -EINVAL ? "1..5 pulses of 20..1000 ms" :
                        ret == -EBUSY  ? "a pattern is still running" :
                        ret == -ENOSYS ? "built without PWM" :
                                         "the pattern did not start",
                        detail);
    }

  deadline = cli_now_ms() + count * (on_ms + off_ms) + 1000u;
  while (vs_haptic_busy() && !cli_expired(deadline))
    {
      usleep(20 * 1000);
    }

  return cli_ok("haptic", st, "count=%u on_ms=%u off_ms=%u", count, on_ms,
                off_ms);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool vs_cli_ui_ready(void)
{
  struct vs_app_status_s st;

  return cli_read(&st) == 0;
}

void vs_cli_usage(void)
{
  printf("Usage: velasight [command]\n"
         "  (none)                run the product UI (started at boot)\n"
         "  status                page, state and both screens\n"
         "  social start          enter the social assistant, wait until "
         "it runs\n"
         "  social stop           end the session, wait for the minutes\n"
         "  social pause|resume   pause or resume the running session\n"
         "  social cancel         abandon a session that is still starting\n"
         "  social retry          collect the minutes again after a "
         "failure\n"
         "  social run [s]        start, run s seconds (30), stop, wait\n"
         "  social tune [default] [k=v ...]  pacing for the next sessions:\n"
         "                        interval max backlog fps poll\n"
         "  social [s]            same as run with a UI up, else the "
         "headless probe\n"
         "  ask                   voice question about the record on screen\n"
         "  photo                 photo question from the blank home page\n"
         "  voice done|exit       end the utterance, or the conversation\n"
         "  next | prev           browse: blank home, records, volume\n"
         "  home                  back to a browse page\n"
         "  volume up|down|<n>    change and save the volume (0..100)\n"
         "  net ap|sta            hotspot on, or back to the Wi-Fi\n"
         "  key <k> [g]           k confirm|back|next|combo, "
         "g short|long|hold|cancel\n"
         "  wait <page,...> [s]   until the UI shows one of the pages\n"
         "  haptic [n [on [off]]] vibrate now: n pulses of on ms, off ms "
         "apart\n"
         "                        (the emotion alert's pattern by "
         "default)\n"
         "  cloudprobe            one cloud round trip, synthetic payload\n"
         "Each command ends with \"vs_cli: ok ...\" or "
         "\"vs_cli: fail ...\".\n");
}

int vs_cli_main(int argc, char *argv[])
{
  struct vs_app_status_s st;
  const char *cmd;
  int ret;

  if (argc < 2 || strcmp(argv[1], "help") == 0)
    {
      vs_cli_usage();
      return argc < 2 ? 2 : 0;
    }

  cmd = argv[1];
  memset(&st, 0, sizeof(st));

  /* The one that drives hardware directly, and so works with no UI too. */

  if (strcmp(cmd, "haptic") == 0)
    {
      const struct vs_app_status_s *ui = cli_read(&st) == 0 ? &st : NULL;

      return cli_haptic(argc, argv, ui);
    }

  if (cli_read(&st) < 0)
    {
      if (strcmp(cmd, "status") != 0 && strcmp(cmd, "social") != 0 &&
          strcmp(cmd, "ask") != 0 && strcmp(cmd, "photo") != 0 &&
          strcmp(cmd, "voice") != 0 && strcmp(cmd, "next") != 0 &&
          strcmp(cmd, "prev") != 0 && strcmp(cmd, "home") != 0 &&
          strcmp(cmd, "volume") != 0 && strcmp(cmd, "net") != 0 &&
          strcmp(cmd, "key") != 0 && strcmp(cmd, "wait") != 0)
        {
          vs_cli_usage();
          return 2;
        }

      return cli_fail(cmd, NULL, "the UI is not running");
    }

  if (strcmp(cmd, "status") == 0)
    {
      cli_print_status(&st);
      return cli_done(cmd, &st);
    }

  if (strcmp(cmd, "social") == 0)
    {
      return cli_social(argc, argv, &st);
    }

  if (strcmp(cmd, "ask") == 0 || strcmp(cmd, "photo") == 0 ||
      strcmp(cmd, "voice") == 0)
    {
      return cli_voice(argc, argv, &st);
    }

  if (strcmp(cmd, "next") == 0 || strcmp(cmd, "prev") == 0)
    {
      return cli_browse(cmd[0] == 'n', &st);
    }

  if (strcmp(cmd, "home") == 0)
    {
      ret = cli_home(&st);
      return ret == 0 ? cli_done(cmd, &st) :
                        cli_fail(cmd, &st, cli_home_why(ret));
    }

  if (strcmp(cmd, "volume") == 0)
    {
      return cli_volume(argc, argv, &st);
    }

  if (strcmp(cmd, "net") == 0)
    {
      return cli_net(argc, argv, &st);
    }

  if (strcmp(cmd, "key") == 0)
    {
      return cli_key(argc, argv, &st);
    }

  if (strcmp(cmd, "wait") == 0)
    {
      return cli_wait_cmd(argc, argv, &st);
    }

  vs_cli_usage();
  return 2;
}
