/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <stdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

gboolean
rv_random_bytes(guint8 *buffer, gsize length, GError **error)
{
#ifdef G_OS_WIN32
    if (BCryptGenRandom(NULL, buffer, (ULONG)length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_RANDOM,
                            "The system random number generator failed.");
        return FALSE;
    }
    return TRUE;
#else
    FILE *file = fopen("/dev/urandom", "rb");
    gboolean ok;

    if (file == NULL) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_RANDOM,
                            "Could not open /dev/urandom.");
        return FALSE;
    }
    ok = fread(buffer, 1, length, file) == length;
    fclose(file);
    if (!ok)
        g_set_error_literal(error, RV_ERROR, RV_ERROR_RANDOM,
                            "Could not read enough random bytes.");
    return ok;
#endif
}

gchar *
rv_random_string(const gchar *alphabet, gsize length, GError **error)
{
    gsize n, filled = 0;
    guint limit;
    gchar *out;
    guint8 block[64];

    g_return_val_if_fail(alphabet != NULL && *alphabet != '\0', NULL);
    n = strlen(alphabet);
    g_return_val_if_fail(n <= 256, NULL);

    /* Reject bytes that would favour the first characters of the alphabet. */
    limit = 256 - (256 % n);
    out = g_malloc(length + 1);

    while (filled < length) {
        if (!rv_random_bytes(block, sizeof block, error)) {
            rw_secure_free(out);
            return NULL;
        }
        for (gsize i = 0; i < sizeof block && filled < length; i++) {
            if (block[i] < limit)
                out[filled++] = alphabet[block[i] % n];
        }
    }
    out[length] = '\0';
    memset(block, 0, sizeof block);
    return out;
}
