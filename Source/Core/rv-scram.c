/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <string.h>

#define SCRAM_ITERATIONS 4096
#define SCRAM_SALT_BYTES 16
#define DIGEST 32

static void
hmac(const guint8 *key, gsize key_len, const guint8 *data, gsize data_len, guint8 out[DIGEST])
{
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA256, key, key_len);
    gsize len = DIGEST;

    g_hmac_update(h, data, (gssize)data_len);
    g_hmac_get_digest(h, out, &len);
    g_hmac_unref(h);
}

/* PBKDF2 with HMAC-SHA-256, one block (RFC 5802, Hi). */
static void
hi(const gchar *password, const guint8 *salt, gsize salt_len, guint iterations, guint8 out[DIGEST])
{
    guint8 u[DIGEST], block[DIGEST];
    guint8 *first = g_malloc(salt_len + 4);

    memcpy(first, salt, salt_len);
    first[salt_len] = 0;
    first[salt_len + 1] = 0;
    first[salt_len + 2] = 0;
    first[salt_len + 3] = 1;
    hmac((const guint8 *)password, strlen(password), first, salt_len + 4, u);
    g_free(first);
    memcpy(block, u, DIGEST);
    for (guint i = 1; i < iterations; i++) {
        hmac((const guint8 *)password, strlen(password), u, DIGEST, u);
        for (guint j = 0; j < DIGEST; j++)
            block[j] ^= u[j];
    }
    memcpy(out, block, DIGEST);
    memset(u, 0, sizeof u);
}

void
rv_scram_derive(const gchar *password, const guint8 *salt, gsize salt_len, guint iterations,
                guint8 stored_key[32], guint8 server_key[32])
{
    guint8 salted[DIGEST], client_key[DIGEST];
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    gsize len = DIGEST;

    hi(password, salt, salt_len, iterations, salted);
    hmac(salted, DIGEST, (const guint8 *)"Client Key", 10, client_key);
    g_checksum_update(sum, client_key, DIGEST);
    g_checksum_get_digest(sum, stored_key, &len);
    g_checksum_free(sum);
    hmac(salted, DIGEST, (const guint8 *)"Server Key", 10, server_key);
    memset(salted, 0, sizeof salted);
    memset(client_key, 0, sizeof client_key);
}

static gboolean
is_ascii(const gchar *s)
{
    for (; *s != '\0'; s++) {
        if ((guchar)*s >= 0x80)
            return FALSE;
    }
    return TRUE;
}

static gchar *
build(const gchar *password, const guint8 *salt, gsize salt_len, guint iterations)
{
    guint8 stored[DIGEST], server[DIGEST];
    gchar *salt64, *stored64, *server64, *out;

    rv_scram_derive(password, salt, salt_len, iterations, stored, server);
    salt64 = g_base64_encode(salt, salt_len);
    stored64 = g_base64_encode(stored, DIGEST);
    server64 = g_base64_encode(server, DIGEST);
    out = g_strdup_printf("SCRAM-SHA-256$%u:%s$%s:%s", iterations, salt64, stored64, server64);
    g_free(salt64);
    g_free(stored64);
    g_free(server64);
    memset(stored, 0, sizeof stored);
    memset(server, 0, sizeof server);
    return out;
}

gchar *
rv_scram_verifier(const gchar *password, GError **error)
{
    guint8 salt[SCRAM_SALT_BYTES];
    gchar *out;

    g_return_val_if_fail(password != NULL, NULL);
    /* Postgres normalises other characters with SASLprep. Only plain ASCII is handled here. */
    if (!is_ascii(password) || *password == '\0') {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_INVALID,
                            "The password is empty or has characters other than plain ASCII.");
        return NULL;
    }
    if (!rv_random_bytes(salt, sizeof salt, error))
        return NULL;
    out = build(password, salt, sizeof salt, SCRAM_ITERATIONS);
    memset(salt, 0, sizeof salt);
    return out;
}

gboolean
rv_scram_verify(const gchar *password, const gchar *verifier)
{
    gchar **parts, **keys;
    guint iterations;
    gsize salt_len = 0;
    guchar *salt;
    gchar *again;
    gboolean ok = FALSE;

    if (password == NULL || verifier == NULL || !g_str_has_prefix(verifier, "SCRAM-SHA-256$")
        || !is_ascii(password))
        return FALSE;
    parts = g_strsplit(verifier + strlen("SCRAM-SHA-256$"), "$", 2);
    if (g_strv_length(parts) != 2) {
        g_strfreev(parts);
        return FALSE;
    }
    keys = g_strsplit(parts[0], ":", 2);
    if (g_strv_length(keys) != 2 || (iterations = (guint)g_ascii_strtoull(keys[0], NULL, 10)) == 0) {
        g_strfreev(keys);
        g_strfreev(parts);
        return FALSE;
    }
    salt = g_base64_decode(keys[1], &salt_len);
    again = build(password, salt, salt_len, iterations);
    ok = g_strcmp0(again, verifier) == 0;
    g_free(again);
    g_free(salt);
    g_strfreev(keys);
    g_strfreev(parts);
    return ok;
}
