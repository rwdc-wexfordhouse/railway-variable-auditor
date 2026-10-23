/*
 * Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
 * Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
 */
#ifndef RVA_INSTALL_H
#define RVA_INSTALL_H

#include "payload.h"

typedef struct {
    wchar_t dir[MAX_PATH];
    BOOL add_to_path;   /* put the install folder on the user PATH for the command line tool */
    BOOL start_menu;
    BOOL desktop;
} InstallOptions;

/* Called with a value from 0 to 100 as files are written. */
typedef void (*progress_fn)(int percent, void *ctx);

void install_defaults(InstallOptions *o);
BOOL install_run(Payload *p, const InstallOptions *o, progress_fn progress, void *ctx, wchar_t *err, DWORD err_len);

/* Reads <dir>\uninstall.dat and removes everything the installer added. */
BOOL uninstall_run(const wchar_t *dir, wchar_t *err, DWORD err_len);

/* The folder of a previous install from the uninstall registry key, or FALSE. */
BOOL install_find_existing(wchar_t *dir, DWORD len);

BOOL install_dir_on_path(const wchar_t *dir);

#endif
