/*
 * A fake Railway CLI with invented data, for testing railway-variable-audit on
 * Windows under Wine (where there is no Python). The same data as fake-railway.py.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE "\"pageInfo\":{\"hasNextPage\":false,\"endCursor\":null}"

/* The value of "key":"..." in text, or an empty string. */
static void
json_string(const char *text, const char *key, char *out, size_t size)
{
    char pattern[64];
    const char *p, *end;

    snprintf(pattern, sizeof pattern, "\"%s\":\"", key);
    out[0] = '\0';
    p = strstr(text, pattern);
    if (p == NULL)
        return;
    p += strlen(pattern);
    end = strchr(p, '"');
    if (end != NULL && (size_t)(end - p) < size) {
        memcpy(out, p, (size_t)(end - p));
        out[end - p] = '\0';
    }
}

int
main(int argc, char **argv)
{
    char input[8192] = "", workspace[128], project[128];
    size_t used;

    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        puts("railway 9.9.9");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "whoami") == 0) {
        puts("{\"name\":\"Test Admin\",\"email\":\"admin@example.invalid\"}");
        return 0;
    }
    if (argc < 3 || strcmp(argv[1], "api") != 0)
        return 1;

    used = fread(input, 1, sizeof input - 1, stdin);
    input[used] = '\0';
    json_string(input, "workspaceId", workspace, sizeof workspace);
    json_string(input, "projectId", project, sizeof project);

    if (strstr(argv[2], "me {") != NULL)
        printf("{\"data\":{\"me\":{\"workspaces\":[{\"id\":\"w1\",\"name\":\"Alpha\"},{\"id\":\"w2\",\"name\":\"Beta\"}]}}}\n");
    else if (strstr(argv[2], "projects(") != NULL)
        printf("{\"data\":{\"projects\":{\"edges\":[{\"node\":{\"id\":\"p-%s\",\"name\":\"Proj %s\"}}],%s}}}\n",
               workspace, workspace, PAGE);
    else if (strstr(argv[2], "services(") != NULL)
        printf("{\"data\":{\"project\":{\"services\":{\"edges\":[{\"node\":{\"id\":\"s1\",\"name\":\"web\"}}],%s}}}}\n", PAGE);
    else if (strstr(argv[2], "environments(") != NULL)
        printf("{\"data\":{\"project\":{\"environments\":{\"edges\":[{\"node\":{\"id\":\"e1\",\"name\":\"production\"}}],%s}}}}\n", PAGE);
    else if (strstr(argv[2], "variables(") != NULL) {
        if (strstr(input, "\"serviceId\"") != NULL)
            printf("{\"data\":{\"variables\":{\"API_TOKEN\":\"tok,with\\\"quote-%s\"}}}\n", project);
        else
            printf("{\"data\":{\"variables\":{\"SESSION_SECRET\":\"0123456789abcdef0123456789abcdef\"}}}\n");
    } else
        printf("{\"data\":{}}\n");
    return 0;
}
