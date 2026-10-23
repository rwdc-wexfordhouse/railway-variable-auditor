/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <string.h>

void
rv_pgurl_free(RvPgUrl *url)
{
    if (url == NULL)
        return;
    g_free(url->user);
    rw_secure_free(url->password);
    g_free(url->host);
    g_free(url->database);
    g_free(url);
}

static gboolean
has_scheme(const gchar *value, gsize *skip)
{
    if (g_ascii_strncasecmp(value, "postgresql://", 13) == 0) {
        *skip = 13;
        return TRUE;
    }
    if (g_ascii_strncasecmp(value, "postgres://", 11) == 0) {
        *skip = 11;
        return TRUE;
    }
    return FALSE;
}

RvPgUrl *
rv_pgurl_parse(const gchar *value)
{
    gsize skip;
    const gchar *rest, *end, *at, *colon, *host_start, *host_end, *slash;
    gchar *authority, *userinfo;
    RvPgUrl *url;

    if (value == NULL || !has_scheme(value, &skip))
        return NULL;
    rest = value + skip;
    end = rest + strcspn(rest, "/?#");
    authority = g_strndup(rest, (gsize)(end - rest));
    at = strrchr(authority, '@');
    if (at == NULL) {
        g_free(authority);
        return NULL;
    }
    userinfo = g_strndup(authority, (gsize)(at - authority));
    colon = strchr(userinfo, ':');
    if (colon == NULL || colon == userinfo || colon[1] == '\0') {
        g_free(userinfo);
        g_free(authority);
        return NULL;
    }

    url = g_new0(RvPgUrl, 1);
    {
        gchar *raw_user = g_strndup(userinfo, (gsize)(colon - userinfo));

        url->user = g_uri_unescape_string(raw_user, NULL);
        g_free(raw_user);
    }
    url->password = g_uri_unescape_string(colon + 1, NULL);

    host_start = at + 1;
    if (*host_start == '[') {
        host_end = strchr(host_start, ']');
        host_end = host_end != NULL ? host_end + 1 : host_start + strlen(host_start);
        url->host = g_strndup(host_start, (gsize)(host_end - host_start));
        colon = *host_end == ':' ? host_end : NULL;
    } else {
        host_end = host_start + strcspn(host_start, ":");
        url->host = g_strndup(host_start, (gsize)(host_end - host_start));
        colon = *host_end == ':' ? host_end : NULL;
    }
    url->port = colon != NULL ? (guint)g_ascii_strtoull(colon + 1, NULL, 10) : 5432;
    if (url->port == 0)
        url->port = 5432;

    slash = *end == '/' ? end + 1 : NULL;
    if (slash != NULL) {
        gsize n = strcspn(slash, "?#");

        url->database = g_uri_unescape_segment(slash, slash + n, NULL);
    }
    g_free(userinfo);
    g_free(authority);
    if (url->user == NULL || url->password == NULL || url->host == NULL || *url->host == '\0') {
        rv_pgurl_free(url);
        return NULL;
    }
    return url;
}

/* Replace every occurrence of `from` with `to`. Returns NULL if there is none. */
static gchar *
replace_all(const gchar *text, const gchar *from, const gchar *to)
{
    GString *out;
    const gchar *p = text, *hit;
    gsize n = strlen(from);
    gboolean any = FALSE;

    if (n == 0)
        return NULL;
    out = g_string_new(NULL);
    while ((hit = strstr(p, from)) != NULL) {
        g_string_append_len(out, p, hit - p);
        g_string_append(out, to);
        p = hit + n;
        any = TRUE;
    }
    if (!any) {
        g_string_free(out, TRUE);
        return NULL;
    }
    g_string_append(out, p);
    return g_string_free(out, FALSE);
}

gchar *
rv_pgurl_replace_password(const gchar *value, const gchar *old_password, const gchar *new_password)
{
    gchar *encoded_old, *encoded_new, *step, *result;

    if (value == NULL || old_password == NULL || *old_password == '\0' || new_password == NULL)
        return NULL;
    encoded_old = g_uri_escape_string(old_password, NULL, FALSE);
    encoded_new = g_uri_escape_string(new_password, NULL, FALSE);

    /* The raw form first, then the percent-encoded form, so both shapes are found. */
    result = replace_all(value, old_password, new_password);
    if (strcmp(encoded_old, old_password) != 0) {
        step = replace_all(result != NULL ? result : value, encoded_old, encoded_new);
        if (step != NULL) {
            g_free(result);
            result = step;
        }
    }
    g_free(encoded_old);
    g_free(encoded_new);
    return result;
}
