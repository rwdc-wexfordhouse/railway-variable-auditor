/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <glib/gstdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

G_DEFINE_QUARK(rv-error-quark, rv_error)

const gchar *
rv_category_name(RvCategory category)
{
    switch (category) {
    case RV_CAT_ROTATE:        return "shared";
    case RV_CAT_DB_ROLE:       return "database-role";
    case RV_CAT_SHARED_MANUAL: return "shared-manual";
    case RV_CAT_SHARED_CONFIG: return "shared-config";
    case RV_CAT_INI:           return "ini-config";
    case RV_CAT_DATABASE:      return "database-credential";
    case RV_CAT_HOSTNAME:      return "internal-hostname";
    case RV_CAT_REFERENCE:     return "reference";
    case RV_CAT_UNSHARED:      return "unshared";
    case RV_CAT_UNCLASSIFIED:
    default:                   return "unclassified";
    }
}

gchar *
rv_now_iso(void)
{
    GDateTime *now = g_date_time_new_now_utc();
    gchar *text = g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");

    g_date_time_unref(now);
    return text;
}

gboolean
rv_check_cancelled(GCancellable *cancel, GError **error)
{
    if (cancel != NULL && g_cancellable_is_cancelled(cancel)) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_CANCELLED, "Cancelled.");
        return TRUE;
    }
    return FALSE;
}

void
rv_progress(RvProgressFunc func, gpointer data, gdouble fraction,
            const gchar *format, ...)
{
    va_list args;
    gchar *message;

    if (func == NULL)
        return;
    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);
    func(data, message, fraction);
    g_free(message);
}

void
rv_gstring_wipe_free(GString *string)
{
    if (string == NULL)
        return;
    if (string->len > 0)
        memset(string->str, 0, string->len);
    g_string_free(string, TRUE);
}

gchar *
rv_build_path(const gchar *dir, const gchar *file)
{
    return g_build_filename(dir, file, NULL);
}

gboolean
rv_read_file(const gchar *path, gchar **contents, GError **error)
{
    GError *local = NULL;

    if (!g_file_get_contents(path, contents, NULL, &local)) {
        g_set_error(error, RV_ERROR, RV_ERROR_IO, "Could not read %s: %s",
                    path, local->message);
        g_error_free(local);
        return FALSE;
    }
    return TRUE;
}

gboolean
rv_append_line(const gchar *path, const gchar *line, GError **error)
{
    FILE *file = g_fopen(path, "ab");
    gboolean ok;

    if (file == NULL) {
        g_set_error(error, RV_ERROR, RV_ERROR_IO, "Could not open %s for writing.", path);
        return FALSE;
    }
    ok = fputs(line, file) >= 0 && fputc('\n', file) != EOF && fflush(file) == 0;
#ifdef G_OS_WIN32
    if (ok)
        _commit(_fileno(file));
#else
    if (ok)
        fsync(fileno(file));
#endif
    if (fclose(file) != 0)
        ok = FALSE;
    if (!ok)
        g_set_error(error, RV_ERROR, RV_ERROR_IO, "Could not write to %s.", path);
    return ok;
}

/* ---- CSV ---------------------------------------------------------------- */

static void
append_field(GString *out, const gchar *field)
{
    if (field == NULL)
        field = "";
    if (strpbrk(field, ",\"\r\n") == NULL) {
        g_string_append(out, field);
        return;
    }
    g_string_append_c(out, '"');
    for (const gchar *p = field; *p != '\0'; p++) {
        if (*p == '"')
            g_string_append_c(out, '"');
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
}

void
rv_csv_append_row(GString *out, const gchar *const *fields)
{
    for (guint i = 0; fields[i] != NULL; i++) {
        if (i > 0)
            g_string_append_c(out, ',');
        append_field(out, fields[i]);
    }
    g_string_append(out, "\r\n");
}

GPtrArray *
rv_csv_parse(const gchar *text, GError **error)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    GPtrArray *fields = g_ptr_array_new();
    GString *field = g_string_new(NULL);
    gboolean quoted = FALSE, was_quoted = FALSE, any = FALSE;

    for (const gchar *p = text; ; p++) {
        gchar c = *p;

        if (quoted) {
            if (c == '\0') {
                g_set_error_literal(error, RV_ERROR, RV_ERROR_CSV,
                                    "A quoted CSV field is not closed.");
                goto fail;
            }
            if (c == '"') {
                if (p[1] == '"') {
                    g_string_append_c(field, '"');
                    p++;
                } else {
                    quoted = FALSE;
                }
            } else {
                g_string_append_c(field, c);
            }
            continue;
        }

        if (c == '"' && field->len == 0 && !was_quoted) {
            quoted = TRUE;
            was_quoted = TRUE;
            any = TRUE;
        } else if (c == ',') {
            g_ptr_array_add(fields, g_string_free(field, FALSE));
            field = g_string_new(NULL);
            was_quoted = FALSE;
            any = TRUE;
        } else if (c == '\r') {
            continue;
        } else if (c == '\n' || c == '\0') {
            if (any || field->len > 0 || fields->len > 0) {
                g_ptr_array_add(fields, g_string_free(field, FALSE));
                field = g_string_new(NULL);
                g_ptr_array_add(fields, NULL);
                g_ptr_array_add(rows, g_ptr_array_free(fields, FALSE));
                fields = g_ptr_array_new();
            }
            was_quoted = FALSE;
            any = FALSE;
            if (c == '\0')
                break;
        } else {
            g_string_append_c(field, c);
            any = TRUE;
        }
    }
    g_string_free(field, TRUE);
    g_ptr_array_free(fields, TRUE);
    return rows;

fail:
    g_string_free(field, TRUE);
    for (guint i = 0; i < fields->len; i++)
        g_free(fields->pdata[i]);
    g_ptr_array_free(fields, TRUE);
    g_ptr_array_unref(rows);
    return NULL;
}

#ifdef G_OS_WIN32
/* Remove inherited permissions and grant the current user only. Not yet tested on Windows. */
static gboolean
restrict_to_owner(const gchar *path)
{
    const gchar *user = g_get_user_name();
    gchar *grant = g_strdup_printf("%s:F", user);
    gchar *argv[] = { "icacls", (gchar *)path, "/inheritance:r", "/grant:r", grant, NULL };
    gint status = -1;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                               | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL)
                  && g_spawn_check_wait_status(status, NULL);

    g_free(grant);
    return ok;
}
#endif

gboolean
rv_write_file(const gchar *path, const gchar *data, gsize length,
              gboolean private_file, GError **error)
{
    GError *local = NULL;

    if (!g_file_set_contents_full(path, data, (gssize)length,
                                  G_FILE_SET_CONTENTS_CONSISTENT,
                                  private_file ? 0600 : 0644, &local)) {
        g_set_error(error, RV_ERROR, RV_ERROR_IO, "Could not write %s: %s",
                    path, local->message);
        g_error_free(local);
        return FALSE;
    }
#ifdef G_OS_WIN32
    if (private_file && !restrict_to_owner(path)) {
        /* Better no file than a file with live secrets that anyone can read. */
        g_remove(path);
        g_set_error(error, RV_ERROR, RV_ERROR_IO,
                    "Could not restrict %s to its owner, so it was not kept.", path);
        return FALSE;
    }
#endif
    return TRUE;
}
