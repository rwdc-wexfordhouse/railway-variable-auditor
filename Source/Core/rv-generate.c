/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <math.h>
#include <string.h>

#define RV_MIN_SECRET_LENGTH 16

typedef enum {
    FMT_UNKNOWN, FMT_UUID, FMT_HEX, FMT_DIGITS, FMT_BASE64, FMT_BASE64URL, FMT_ALNUM
} Format;

/*
 * Names are matched by whole words, split at anything that is not a letter or
 * a digit, so JWT_AUDIENCE, BYPASS and DESIGN do not match SECRET words. A
 * phrase such as ACCESS_KEY matches two words in a row.
 */
static const gchar *const secretish_words[] = {
    "SECRET", "KEY", "TOKEN", "PASSWORD", "PASSWD", "PASS", "PWD", "SALT",
    "SIGNING", "PRIVATE", "CREDENTIAL", "CREDENTIALS", "PEPPER", NULL
};

static const gchar *const internal_words[] = {
    "SECRET", "SALT", "PEPPER", "HMAC", "ENCRYPTION", "CSRF", "NEXTAUTH",
    "SIGNING", "SESSION", "COOKIE", "APP_KEY", "INTERNAL_TOKEN", "SERVICE_TOKEN", NULL
};

/* A name that suggests a key that belongs to another company. */
static const gchar *const provider_words[] = {
    "API_KEY", "APIKEY", "ACCESS_KEY", "CLIENT_SECRET", "CLIENT_ID", "WEBHOOK",
    "STRIPE", "OPENAI", "ANTHROPIC", "GROK", "OPENROUTER", "AWS", "GOOGLE",
    "GCP", "GITHUB", "SENDGRID", "TWILIO", "XERO", "SHOPIFY", "SLACK",
    "MAILGUN", "RESEND", "SUPABASE", "CLOUDFLARE", "AZURE", "SENTRY", "PAYPAL",
    "PAYFAST", "MAILCHIMP", "POSTMARK", "DISCORD", "TELEGRAM", NULL
};

static const gchar *const provider_prefixes[] = {
    "sk-", "sk_", "pk_", "rk_", "ghp_", "gho_", "ghu_", "ghs_", "ghr_",
    "github_pat_", "xoxb-", "xoxp-", "xoxa-", "AKIA", "ASIA", "AIza", "SG.",
    "shpat_", "shpss_", "whsec_", "glpat-", "npm_", "re_", NULL
};

static GPtrArray *
split_words(const gchar *text)
{
    GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
    GString *current = g_string_new(NULL);

    for (const gchar *p = text; ; p++) {
        if (*p != '\0' && g_ascii_isalnum(*p)) {
            g_string_append_c(current, g_ascii_toupper(*p));
        } else {
            if (current->len > 0)
                g_ptr_array_add(words, g_strdup(current->str));
            g_string_truncate(current, 0);
            if (*p == '\0')
                break;
        }
    }
    g_string_free(current, TRUE);
    return words;
}

static gboolean
has_phrase(const GPtrArray *words, const gchar *phrase)
{
    gchar **parts = g_strsplit(phrase, "_", -1);
    guint n = g_strv_length(parts);
    gboolean found = FALSE;

    for (guint start = 0; start + n <= words->len && !found; start++) {
        gboolean match = TRUE;

        for (guint i = 0; i < n && match; i++)
            match = strcmp(words->pdata[start + i], parts[i]) == 0;
        found = match;
    }
    g_strfreev(parts);
    return found;
}

static gboolean
name_has_word(const gchar *name, const gchar *const *phrases)
{
    GPtrArray *words;
    gboolean found = FALSE;

    if (name == NULL)
        return FALSE;
    words = split_words(name);
    for (guint i = 0; phrases[i] != NULL && !found; i++)
        found = has_phrase(words, phrases[i]);
    g_ptr_array_unref(words);
    return found;
}

gboolean
rv_name_is_secretish(const gchar *name)
{
    return name_has_word(name, secretish_words);
}

gboolean
rv_name_is_internal_secret(const gchar *name)
{
    return name_has_word(name, internal_words);
}

gboolean
rv_name_is_provider(const gchar *name)
{
    return name_has_word(name, provider_words);
}

gboolean
rv_value_has_provider_prefix(const gchar *value)
{
    if (value == NULL)
        return FALSE;
    for (guint i = 0; provider_prefixes[i] != NULL; i++) {
        if (g_str_has_prefix(value, provider_prefixes[i]))
            return TRUE;
    }
    return FALSE;
}

static gdouble
entropy_per_char(const gchar *value)
{
    guint counts[256] = { 0 };
    gsize length = strlen(value);
    gdouble bits = 0;

    if (length == 0)
        return 0;
    for (gsize i = 0; i < length; i++)
        counts[(guchar)value[i]]++;
    for (guint c = 0; c < 256; c++) {
        if (counts[c] > 0) {
            gdouble p = (gdouble)counts[c] / (gdouble)length;

            bits -= p * log2(p);
        }
    }
    return bits;
}

static gboolean
has_space_or_url(const gchar *value)
{
    return strpbrk(value, " \t\r\n") != NULL || strstr(value, "://") != NULL;
}

gboolean
rv_looks_like_secret(const gchar *name, const gchar *value)
{
    gsize length;

    if (value == NULL || *value == '\0')
        return FALSE;
    length = strlen(value);

    if (has_space_or_url(value))
        return FALSE;
    if (rv_name_is_secretish(name) && length >= 8)
        return TRUE;
    if (length < 20)
        return FALSE;
    return entropy_per_char(value) >= 3.5;
}

/* Format detection */

static gboolean
all_chars(const gchar *s, gboolean (*pred)(gchar))
{
    for (; *s != '\0'; s++) {
        if (!pred(*s))
            return FALSE;
    }
    return TRUE;
}

static gboolean is_hex(gchar c) { return g_ascii_isxdigit(c); }
static gboolean is_digit(gchar c) { return g_ascii_isdigit(c); }
static gboolean is_alnum(gchar c) { return g_ascii_isalnum(c); }
static gboolean is_b64(gchar c) { return g_ascii_isalnum(c) || c == '+' || c == '/'; }
static gboolean is_b64url(gchar c) { return g_ascii_isalnum(c) || c == '-' || c == '_'; }

static gboolean
is_uuid(const gchar *v)
{
    if (strlen(v) != 36)
        return FALSE;
    for (guint i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (v[i] != '-')
                return FALSE;
        } else if (!g_ascii_isxdigit(v[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

static Format
detect(const gchar *v, gsize *padding)
{
    gsize length = strlen(v), body;
    gsize pad = 0;
    gboolean letter = FALSE;

    *padding = 0;
    if (length < RV_MIN_SECRET_LENGTH)
        return FMT_UNKNOWN;
    if (is_uuid(v))
        return FMT_UUID;

    for (const gchar *p = v; *p != '\0'; p++) {
        if (g_ascii_isalpha(*p))
            letter = TRUE;
    }
    if (all_chars(v, is_digit))
        return FMT_DIGITS;
    if (all_chars(v, is_hex) && letter)
        return FMT_HEX;

    /* Trailing '=' is base64 padding. */
    while (pad < 2 && pad < length && v[length - 1 - pad] == '=')
        pad++;
    body = length - pad;
    if (pad > 0 || strpbrk(v, "+/") != NULL) {
        gchar *head = g_strndup(v, body);
        gboolean ok = body > 0 && all_chars(head, is_b64);

        g_free(head);
        if (ok) {
            *padding = pad;
            return FMT_BASE64;
        }
        return FMT_UNKNOWN;
    }
    if (all_chars(v, is_alnum))
        return FMT_ALNUM;
    if (all_chars(v, is_b64url))
        return FMT_BASE64URL;
    return FMT_UNKNOWN;
}

gboolean
rv_format_supported(const gchar *value)
{
    gsize pad;

    return value != NULL && detect(value, &pad) != FMT_UNKNOWN;
}

static gboolean
has_upper_only(const gchar *v)
{
    gboolean upper = FALSE, lower = FALSE;

    for (; *v != '\0'; v++) {
        if (g_ascii_isupper(*v))
            upper = TRUE;
        if (g_ascii_islower(*v))
            lower = TRUE;
    }
    return upper && !lower;
}

gchar *
rv_generate_like(const gchar *old_value, GError **error)
{
    static const gchar hex_lower[] = "0123456789abcdef";
    static const gchar hex_upper[] = "0123456789ABCDEF";
    static const gchar digits[] = "0123456789";
    static const gchar alnum[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static const gchar b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const gchar b64url[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    gsize length, pad;
    Format format;

    g_return_val_if_fail(old_value != NULL, NULL);
    length = strlen(old_value);
    format = detect(old_value, &pad);

    switch (format) {
    case FMT_UUID: {
        guint8 b[16];
        gchar *out;

        if (!rv_random_bytes(b, sizeof b, error))
            return NULL;
        b[6] = (b[6] & 0x0f) | 0x40;
        b[8] = (b[8] & 0x3f) | 0x80;
        out = g_strdup_printf(has_upper_only(old_value)
            ? "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X"
            : "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        memset(b, 0, sizeof b);
        return out;
    }
    case FMT_HEX:
        return rv_random_string(has_upper_only(old_value) ? hex_upper : hex_lower,
                                length, error);
    case FMT_DIGITS:
        return rv_random_string(digits, length, error);
    case FMT_ALNUM:
        return rv_random_string(alnum, length, error);
    case FMT_BASE64URL:
        return rv_random_string(b64url, length, error);
    case FMT_BASE64: {
        gchar *body = rv_random_string(b64, length - pad, error);
        gchar *out;

        if (body == NULL)
            return NULL;
        if (pad > 0) {
            /* With padding, the last character carries fewer bits. A strict decoder
             * rejects a last character whose unused bits are not zero. */
            static const gchar last_one_pad[] = "AEIMQUYcgkosw048";   /* index multiple of 4 */
            static const gchar last_two_pad[] = "AQgw";               /* index multiple of 16 */
            gchar *last = rv_random_string(pad == 1 ? last_one_pad : last_two_pad, 1, error);

            if (last == NULL) {
                rw_secure_free(body);
                return NULL;
            }
            body[length - pad - 1] = last[0];
            rw_secure_free(last);
        }
        out = g_strdup_printf("%s%.*s", body, (int)pad, "==");
        rw_secure_free(body);
        return out;
    }
    case FMT_UNKNOWN:
    default:
        g_set_error_literal(error, RV_ERROR, RV_ERROR_INVALID,
                            "The value is not in a format the tool can regenerate.");
        return NULL;
    }
}
