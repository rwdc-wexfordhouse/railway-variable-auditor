/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "railway-private.h"

#include <gio/gio.h>
#include <string.h>

#define RW_CLI_INSTALL_HINT \
    "The Railway CLI was not found. Install it (brew install railway, " \
    "scoop install railway or npm i -g @railway/cli) and try again."

static gchar *cli_program = NULL;

void
rw_cli_set_program(const gchar *path)
{
    g_free(cli_program);
    cli_program = g_strdup(path);
}

gchar *
rw_cli_find(void)
{
    const gchar *from_environment = g_getenv("RVC_RAILWAY_CLI");

    if (cli_program != NULL)
        return g_strdup(cli_program);
    if (from_environment != NULL && *from_environment != '\0')
        return g_strdup(from_environment);
    return g_find_program_in_path("railway");
}

/*
 * Run the CLI. Captures stdout and stderr, feeds stdin_text (may be NULL).
 * Returns FALSE with RW_ERROR_CLI when it cannot be run at all.
 */
static gboolean
run_cli(const gchar *const *args, const gchar *token, const gchar *stdin_text,
        gchar **out, gchar **err, gint *exit_status, GError **error)
{
    gchar *program = rw_cli_find();
    GPtrArray *argv;
    GSubprocessLauncher *launcher;
    GSubprocess *process;
    GError *local = NULL;
    gboolean ok;

    if (program == NULL) {
        g_set_error_literal(error, RW_ERROR, RW_ERROR_CLI, RW_CLI_INSTALL_HINT);
        return FALSE;
    }

    argv = g_ptr_array_new();
    g_ptr_array_add(argv, program);
    for (; *args != NULL; args++)
        g_ptr_array_add(argv, (gpointer)*args);
    g_ptr_array_add(argv, NULL);

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE
                                         | G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                         | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    if (token != NULL)
        g_subprocess_launcher_setenv(launcher, "RAILWAY_API_TOKEN", token, TRUE);

    process = g_subprocess_launcher_spawnv(launcher,
                                           (const gchar *const *)argv->pdata, &local);
    g_object_unref(launcher);
    g_ptr_array_free(argv, TRUE);
    g_free(program);

    if (process == NULL) {
        g_set_error(error, RW_ERROR, RW_ERROR_CLI,
                    "Could not run the Railway CLI: %s", local->message);
        g_error_free(local);
        return FALSE;
    }

    ok = g_subprocess_communicate_utf8(process, stdin_text, NULL, out, err, &local);
    if (!ok) {
        g_set_error(error, RW_ERROR, RW_ERROR_CLI,
                    "The Railway CLI did not finish cleanly: %s", local->message);
        g_error_free(local);
    } else {
        *exit_status = g_subprocess_get_if_exited(process)
                           ? g_subprocess_get_exit_status(process) : -1;
    }
    g_object_unref(process);
    return ok;
}

/* Request splitting and failure mapping */

gboolean
rw_cli_split_request(const gchar *body, gchar **query, gchar **variables_json,
                     GError **error)
{
    JsonParser *parser = json_parser_new();
    JsonNode *root;
    JsonObject *object;
    JsonNode *variables;
    const gchar *text;

    *query = NULL;
    *variables_json = NULL;

    if (!json_parser_load_from_data(parser, body, -1, NULL)
        || (root = json_parser_get_root(parser)) == NULL
        || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, RW_ERROR, RW_ERROR_INVALID,
                            "The request is not a JSON object.");
        g_object_unref(parser);
        return FALSE;
    }

    object = json_node_get_object(root);
    text = json_object_has_member(object, "query")
               ? json_object_get_string_member(object, "query") : NULL;
    if (text == NULL) {
        g_set_error_literal(error, RW_ERROR, RW_ERROR_INVALID,
                            "The request has no query.");
        g_object_unref(parser);
        return FALSE;
    }
    *query = g_strdup(text);

    variables = json_object_get_member(object, "variables");
    if (variables != NULL && JSON_NODE_HOLDS_OBJECT(variables)) {
        JsonGenerator *generator = json_generator_new();

        json_generator_set_root(generator, variables);
        *variables_json = json_generator_to_data(generator, NULL);
        g_object_unref(generator);
    }
    g_object_unref(parser);
    return TRUE;
}

/*
 * The CLI prints "Railway API request failed with HTTP <code> <text>" on
 * stderr. Returns the code, 401 for a missing login, or 0 if unknown.
 */
gint
rw_cli_status_from_failure(const gchar *stderr_text)
{
    const gchar *at;

    if (stderr_text == NULL)
        return 0;

    at = strstr(stderr_text, "HTTP ");
    if (at != NULL && g_ascii_isdigit(at[5]) && g_ascii_isdigit(at[6])
        && g_ascii_isdigit(at[7]))
        return (at[5] - '0') * 100 + (at[6] - '0') * 10 + (at[7] - '0');

    if (strstr(stderr_text, "Unauthorized") != NULL
        || strstr(stderr_text, "railway login") != NULL)
        return 401;
    return 0;
}

/* Default transport */

gboolean
rw_cli_transport_send(gpointer user_data, const gchar *url, const gchar *token,
                      const gchar *body, RwHttpResponse *response, GError **error)
{
    gchar *query = NULL, *variables = NULL, *out = NULL, *err = NULL;
    gint exit_status = 0;
    const gchar *args[7];
    guint n = 0;
    gboolean ok;

    (void)user_data;
    (void)url;

    if (!rw_cli_split_request(body, &query, &variables, error))
        return FALSE;

    args[n++] = "api";
    args[n++] = query;
    args[n++] = "--compact";
    args[n++] = "--allow-errors";
    if (variables != NULL) {
        args[n++] = "--variables";
        args[n++] = "@-";
    }
    args[n] = NULL;

    ok = run_cli(args, token, variables, &out, &err, &exit_status, error);
    g_free(query);
    rw_secure_free(variables);

    if (ok) {
        response->body = out;
        out = NULL;
        if (exit_status == 0) {
            response->status = 200;
        } else {
            response->status = rw_cli_status_from_failure(err);
            if (response->status == 0) {
                gchar *line = err != NULL ? g_strndup(err, strcspn(err, "\r\n")) : g_strdup("");

                g_set_error(error, RW_ERROR, RW_ERROR_CLI,
                            "The Railway CLI failed: %.200s", line);
                g_free(line);
                ok = FALSE;
            }
        }
    }
    rw_secure_free(out);
    g_free(err);
    return ok;
}

/* Helpers for the application */

gchar *
rw_cli_version(GError **error)
{
    const gchar *args[] = { "--version", NULL };
    gchar *out = NULL, *err = NULL, *text;
    gint status = 0;

    if (!run_cli(args, NULL, NULL, &out, &err, &status, error))
        return NULL;
    g_free(err);
    if (status != 0 || out == NULL || *out == '\0') {
        g_free(out);
        g_set_error_literal(error, RW_ERROR, RW_ERROR_CLI,
                            "The Railway CLI did not report a version.");
        return NULL;
    }
    text = g_strstrip(out);
    return text;
}

gboolean
rw_cli_whoami(gchar **name, gchar **email, GError **error)
{
    const gchar *args[] = { "whoami", "--json", NULL };
    gchar *out = NULL, *err = NULL;
    gint status = 0;
    JsonParser *parser;
    JsonNode *root;

    if (!run_cli(args, NULL, NULL, &out, &err, &status, error))
        return FALSE;

    if (status != 0) {
        gboolean signed_out = rw_cli_status_from_failure(err) == 401
                              || (out != NULL && strstr(out, "railway login") != NULL);

        g_set_error_literal(error, RW_ERROR, signed_out ? RW_ERROR_AUTH : RW_ERROR_CLI,
                            signed_out ? "Not signed in to Railway. Run railway login."
                                       : "The Railway CLI could not report the user.");
        g_free(out);
        g_free(err);
        return FALSE;
    }
    g_free(err);

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, out, -1, NULL)
        || (root = json_parser_get_root(parser)) == NULL
        || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, RW_ERROR, RW_ERROR_PARSE,
                            "The Railway CLI sent user details in an unexpected shape.");
        g_object_unref(parser);
        g_free(out);
        return FALSE;
    }

    if (name != NULL) {
        JsonObject *object = json_node_get_object(root);
        const gchar *value = json_object_has_member(object, "name")
                                 ? json_object_get_string_member(object, "name") : NULL;

        *name = g_strdup(value);
    }
    if (email != NULL) {
        JsonObject *object = json_node_get_object(root);
        const gchar *value = json_object_has_member(object, "email")
                                 ? json_object_get_string_member(object, "email") : NULL;

        *email = g_strdup(value);
    }
    g_object_unref(parser);
    g_free(out);
    return TRUE;
}

static gboolean
run_interactive(const gchar *command, GError **error)
{
    gchar *program = rw_cli_find();
    const gchar *argv[3];
    GSubprocess *process;
    GError *local = NULL;
    gboolean ok;

    if (program == NULL) {
        g_set_error_literal(error, RW_ERROR, RW_ERROR_CLI, RW_CLI_INSTALL_HINT);
        return FALSE;
    }

    argv[0] = program;
    argv[1] = command;
    argv[2] = NULL;

    /* The child keeps our terminal so it can show the sign-in link. */
    process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &local);
    g_free(program);
    if (process == NULL) {
        g_set_error(error, RW_ERROR, RW_ERROR_CLI,
                    "Could not run the Railway CLI: %s", local->message);
        g_error_free(local);
        return FALSE;
    }

    ok = g_subprocess_wait_check(process, NULL, &local);
    if (!ok) {
        g_set_error(error, RW_ERROR, RW_ERROR_CLI,
                    "railway %s did not complete: %s", command, local->message);
        g_error_free(local);
    }
    g_object_unref(process);
    return ok;
}

gboolean
rw_cli_login(GError **error)
{
    return run_interactive("login", error);
}

gboolean
rw_cli_logout(GError **error)
{
    return run_interactive("logout", error);
}
