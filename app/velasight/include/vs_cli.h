/****************************************************************************
 * app/velasight/include/vs_cli.h
 *
 * nsh commands that operate the running UI.  See vs_cli.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_VELASIGHT_INCLUDE_VS_CLI_H
#define __APP_VELASIGHT_INCLUDE_VS_CLI_H

#include <stdbool.h>

/* Whether a UI is up and taking input, so a command has a UI to drive. */

bool vs_cli_ui_ready(void);

/* Run "velasight <command> ...".  Returns the exit status: 0 when the
 * command's last line read "vs_cli: ok", 1 for "vs_cli: fail", 2 for usage.
 */

int vs_cli_main(int argc, char *argv[]);

/* The command list, for main()'s usage text. */

void vs_cli_usage(void);

#endif /* __APP_VELASIGHT_INCLUDE_VS_CLI_H */
