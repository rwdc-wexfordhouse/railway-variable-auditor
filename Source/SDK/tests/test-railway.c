/*
 * Unit tests for the Railway SDK. They use a mock transport, so there is
 * no network access and no Railway account involved.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "railway.h"
#include "railway-private.h"

#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    gint         status;
    const gchar *body;
    gint         retry_after;
} Canned;

typedef struct {
    Canned  *replies;
    guint    count;
    guint    next;
    GPtrArray *requests;    /* request bodies, owned */
    guint    slept;         /* total seconds "slept" */
} Mock;

static gboolean
mock_send(gpointer user_data, const gchar *url, const gchar *token,
          const gchar *body, RwHttpResponse *response, GError **error)
{
    Mock *mock = user_data;

    (void)url;
    g_assert_cmpstr(token, ==, "test-token");
    g_ptr_array_add(mock->requests, g_strdup(body));

    if (mock->next >= mock->count) {
        g_set_error_literal(error, RW_ERROR, RW_ERROR_NETWORK, "no reply queued");
        return FALSE;
    }

    Canned *reply = &mock->replies[mock->next++];

    response->status = reply->status;
    response->body = g_strdup(reply->body);
    response->retry_after = reply->retry_after;
    response->rate_remaining = 42;
    return TRUE;
}

static void
mock_sleep(gpointer user_data, guint seconds)
{
    ((Mock *)user_data)->slept += seconds;
}

static RwClient *
make_client(Mock *mock, Canned *replies, guint count)
{
    RwClient *client = rw_client_new("test-token");

    mock->replies = replies;
    mock->count = count;
    mock->next = 0;
    mock->slept = 0;
    mock->requests = g_ptr_array_new_with_free_func(g_free);
    rw_client_set_transport(client, mock_send, mock, NULL);
    rw_client_set_sleep(client, mock_sleep, mock);
    return client;
}

static JsonObject *
request_variables(Mock *mock, guint index)
{
    JsonParser *parser = json_parser_new();
    JsonObject *vars;

    g_assert_true(json_parser_load_from_data(parser, mock->requests->pdata[index], -1, NULL));
    vars = json_object_ref(json_object_get_object_member(
        json_node_get_object(json_parser_get_root(parser)), "variables"));
    g_object_unref(parser);
    return vars;
}

static void
test_execute_returns_data(void)
{
    Mock mock;
    Canned replies[] = { { 200, "{\"data\":{\"me\":{\"name\":\"Ross\"}}}", -1 } };
    RwClient *client = make_client(&mock, replies, 1);
    GError *error = NULL;
    JsonNode *data = rw_client_execute(client, "query { me { name } }", NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(data);
    g_assert_cmpuint(rw_client_get_request_count(client), ==, 1);
    g_assert_cmpint(rw_client_get_rate_remaining(client), ==, 42);
    json_node_unref(data);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_graphql_error(void)
{
    Mock mock;
    Canned replies[] = { { 200, "{\"errors\":[{\"message\":\"Problem processing request\"}]}", -1 } };
    RwClient *client = make_client(&mock, replies, 1);
    GError *error = NULL;

    g_assert_null(rw_client_execute(client, "query { x }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_GRAPHQL);
    g_assert_nonnull(strstr(error->message, "Problem processing request"));
    g_error_free(error);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_auth_errors(void)
{
    Mock mock;
    Canned replies[] = {
        { 200, "{\"errors\":[{\"message\":\"Not Authorized\"}]}", -1 },
        { 401, "", -1 },
    };
    RwClient *client = make_client(&mock, replies, 2);
    GError *error = NULL;

    g_assert_null(rw_client_execute(client, "query { x }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_AUTH);
    g_clear_error(&error);
    g_assert_null(rw_client_execute(client, "query { x }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_AUTH);
    g_error_free(error);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_rate_limit_retry(void)
{
    Mock mock;
    Canned replies[] = {
        { 429, "", 7 },
        { 200, "{\"data\":{\"ok\":true}}", -1 },
    };
    RwClient *client = make_client(&mock, replies, 2);
    GError *error = NULL;
    JsonNode *data = rw_client_execute(client, "query { ok }", NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(data);
    g_assert_cmpuint(mock.slept, ==, 7);
    g_assert_cmpuint(rw_client_get_request_count(client), ==, 2);
    json_node_unref(data);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_rate_limit_too_long(void)
{
    Mock mock;
    Canned replies[] = { { 429, "", 3000 } };
    RwClient *client = make_client(&mock, replies, 1);
    GError *error = NULL;

    g_assert_null(rw_client_execute(client, "query { ok }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_RATE_LIMITED);
    g_assert_cmpint(rw_client_get_retry_after(client), ==, 3000);
    g_assert_cmpuint(mock.slept, ==, 0);
    g_error_free(error);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_server_error_retried(void)
{
    Mock mock;
    Canned replies[] = {
        { 502, "", -1 },
        { 200, "{\"data\":{\"ok\":true}}", -1 },
    };
    RwClient *client = make_client(&mock, replies, 2);
    GError *error = NULL;
    JsonNode *data = rw_client_execute(client, "query { ok }", NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(data);
    json_node_unref(data);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_list_workspaces(void)
{
    Mock mock;
    Canned replies[] = { { 200,
        "{\"data\":{\"me\":{\"workspaces\":[{\"id\":\"w1\",\"name\":\"Personal\"},"
        "{\"id\":\"w2\",\"name\":\"Work\"}]}}}", -1 } };
    RwClient *client = make_client(&mock, replies, 1);
    GError *error = NULL;
    GPtrArray *list = rw_list_workspaces(client, &error);

    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(list->len, ==, 2);
    g_assert_cmpstr(((RwEntity *)list->pdata[1])->id, ==, "w2");
    g_assert_cmpstr(((RwEntity *)list->pdata[1])->name, ==, "Work");
    g_ptr_array_unref(list);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_list_projects_pages(void)
{
    Mock mock;
    Canned replies[] = {
        { 200, "{\"data\":{\"projects\":{\"edges\":[{\"node\":{\"id\":\"p1\",\"name\":\"One\"}}],"
               "\"pageInfo\":{\"hasNextPage\":true,\"endCursor\":\"c1\"}}}}", -1 },
        { 200, "{\"data\":{\"projects\":{\"edges\":[{\"node\":{\"id\":\"p2\",\"name\":\"Two\"}}],"
               "\"pageInfo\":{\"hasNextPage\":false,\"endCursor\":null}}}}", -1 },
    };
    RwClient *client = make_client(&mock, replies, 2);
    GError *error = NULL;
    GPtrArray *list = rw_list_projects(client, "w1", &error);

    g_assert_no_error(error);
    g_assert_nonnull(list);
    g_assert_cmpuint(list->len, ==, 2);
    g_assert_cmpstr(((RwEntity *)list->pdata[1])->id, ==, "p2");

    JsonObject *first = request_variables(&mock, 0);
    JsonObject *second = request_variables(&mock, 1);

    g_assert_cmpstr(json_object_get_string_member(first, "workspaceId"), ==, "w1");
    g_assert_false(json_object_has_member(first, "after"));
    g_assert_cmpstr(json_object_get_string_member(second, "after"), ==, "c1");
    json_object_unref(first);
    json_object_unref(second);
    g_ptr_array_unref(list);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_paging_must_advance(void)
{
    Mock mock;
    Canned replies[] = {
        { 200, "{\"data\":{\"projects\":{\"edges\":[],\"pageInfo\":{\"hasNextPage\":true,\"endCursor\":\"c1\"}}}}", -1 },
        { 200, "{\"data\":{\"projects\":{\"edges\":[],\"pageInfo\":{\"hasNextPage\":true,\"endCursor\":\"c1\"}}}}", -1 },
    };
    RwClient *client = make_client(&mock, replies, 2);
    GError *error = NULL;

    g_assert_null(rw_list_projects(client, "w1", &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_PARSE);
    g_error_free(error);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_list_services_and_environments(void)
{
    Mock mock;
    Canned replies[] = {
        { 200, "{\"data\":{\"project\":{\"services\":{\"edges\":[{\"node\":{\"id\":\"s1\",\"name\":\"api\"}}],"
               "\"pageInfo\":{\"hasNextPage\":false}}}}}", -1 },
        { 200, "{\"data\":{\"project\":{\"environments\":{\"edges\":[{\"node\":{\"id\":\"e1\",\"name\":\"production\"}}],"
               "\"pageInfo\":{\"hasNextPage\":false}}}}}", -1 },
    };
    RwClient *client = make_client(&mock, replies, 2);
    GError *error = NULL;
    GPtrArray *services = rw_list_services(client, "p1", &error);
    GPtrArray *environments = rw_list_environments(client, "p1", &error);

    g_assert_no_error(error);
    g_assert_cmpstr(((RwEntity *)services->pdata[0])->name, ==, "api");
    g_assert_cmpstr(((RwEntity *)environments->pdata[0])->name, ==, "production");
    g_ptr_array_unref(services);
    g_ptr_array_unref(environments);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_get_variables(void)
{
    Mock mock;
    Canned replies[] = { { 200,
        "{\"data\":{\"variables\":{\"DB\":\"${{Postgres.DATABASE_URL}}\",\"KEY\":\"abc\"}}}", -1 } };
    RwClient *client = make_client(&mock, replies, 1);
    GError *error = NULL;
    GHashTable *vars = rw_get_variables(client, "p1", "e1", NULL, TRUE, &error);

    g_assert_no_error(error);
    g_assert_nonnull(vars);
    g_assert_cmpuint(g_hash_table_size(vars), ==, 2);
    g_assert_cmpstr(g_hash_table_lookup(vars, "DB"), ==, "${{Postgres.DATABASE_URL}}");

    JsonObject *sent = request_variables(&mock, 0);

    g_assert_false(json_object_has_member(sent, "serviceId"));
    g_assert_true(json_object_get_boolean_member(sent, "unrendered"));
    json_object_unref(sent);
    g_hash_table_unref(vars);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_upsert_never_replaces(void)
{
    Mock mock;
    Canned replies[] = { { 200, "{\"data\":{\"variableCollectionUpsert\":true}}", -1 } };
    RwClient *client = make_client(&mock, replies, 1);
    GError *error = NULL;
    GHashTable *vars = rw_variables_new();

    g_hash_table_insert(vars, g_strdup("SECRET"), g_strdup("new-value"));
    g_assert_true(rw_upsert_variables(client, "p1", "e1", "s1", vars, TRUE, &error));
    g_assert_no_error(error);

    JsonObject *sent = request_variables(&mock, 0);
    JsonObject *input = json_object_get_object_member(sent, "input");
    JsonObject *values = json_object_get_object_member(input, "variables");

    g_assert_cmpstr(json_object_get_string_member(input, "serviceId"), ==, "s1");
    g_assert_false(json_object_get_boolean_member(input, "replace"));
    g_assert_true(json_object_get_boolean_member(input, "skipDeploys"));
    g_assert_cmpstr(json_object_get_string_member(values, "SECRET"), ==, "new-value");
    json_object_unref(sent);
    g_hash_table_unref(vars);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_upsert_shared_omits_service(void)
{
    Mock mock;
    Canned replies[] = { { 200, "{\"data\":{\"variableCollectionUpsert\":true}}", -1 } };
    RwClient *client = make_client(&mock, replies, 1);
    GHashTable *vars = rw_variables_new();

    g_hash_table_insert(vars, g_strdup("A"), g_strdup("b"));
    g_assert_true(rw_upsert_variables(client, "p1", "e1", NULL, vars, FALSE, NULL));

    JsonObject *sent = request_variables(&mock, 0);

    g_assert_false(json_object_has_member(json_object_get_object_member(sent, "input"), "serviceId"));
    json_object_unref(sent);
    g_hash_table_unref(vars);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_upsert_rejects_empty(void)
{
    Mock mock;
    RwClient *client = make_client(&mock, NULL, 0);
    GHashTable *vars = rw_variables_new();
    GError *error = NULL;

    g_assert_false(rw_upsert_variables(client, "p1", "e1", NULL, vars, TRUE, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_INVALID);
    g_assert_cmpuint(rw_client_get_request_count(client), ==, 0);
    g_error_free(error);
    g_hash_table_unref(vars);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_redeploy_service(void)
{
    Mock mock;
    Canned replies[] = { { 200, "{\"data\":{\"serviceInstanceRedeploy\":true}}", -1 } };
    RwClient *client = make_client(&mock, replies, 1);

    g_assert_true(rw_redeploy_service(client, "s1", "e1", NULL));
    g_assert_nonnull(strstr(mock.requests->pdata[0], "serviceInstanceRedeploy"));
    g_assert_null(strstr(mock.requests->pdata[0], "serviceInstanceDeploy"));
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_rate_limit_unknown_wait(void)
{
    Mock mock;
    Canned replies[] = {
        { 429, "", -1 }, { 429, "", -1 }, { 429, "", -1 },
        { 429, "", -1 }, { 429, "", -1 }, { 429, "", -1 },
    };
    RwClient *client = make_client(&mock, replies, 6);
    GError *error = NULL;

    g_assert_null(rw_client_execute(client, "query { ok }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_RATE_LIMITED);
    g_assert_nonnull(strstr(error->message, "did not say"));
    g_error_free(error);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

/* The CLI transport */

static void
test_cli_split_request(void)
{
    gchar *query = NULL, *vars = NULL;
    GError *error = NULL;

    g_assert_true(rw_cli_split_request(
        "{\"query\":\"query { me { id } }\",\"variables\":{\"a\":\"b\"}}",
        &query, &vars, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(query, ==, "query { me { id } }");
    g_assert_cmpstr(vars, ==, "{\"a\":\"b\"}");
    g_free(query);
    g_free(vars);

    g_assert_true(rw_cli_split_request("{\"query\":\"query { x }\"}", &query, &vars, &error));
    g_assert_cmpstr(query, ==, "query { x }");
    g_assert_null(vars);
    g_free(query);

    g_assert_false(rw_cli_split_request("not json", &query, &vars, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_INVALID);
    g_clear_error(&error);
    g_assert_false(rw_cli_split_request("{\"variables\":{}}", &query, &vars, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_INVALID);
    g_error_free(error);
}

static void
test_cli_status_from_failure(void)
{
    g_assert_cmpint(rw_cli_status_from_failure(
        "Railway API request failed with HTTP 429 Too Many Requests"), ==, 429);
    g_assert_cmpint(rw_cli_status_from_failure(
        "Railway API request failed with HTTP 502 Bad Gateway"), ==, 502);
    g_assert_cmpint(rw_cli_status_from_failure(
        "Unauthorized. Please login with `railway login`."), ==, 401);
    g_assert_cmpint(rw_cli_status_from_failure("something broke"), ==, 0);
    g_assert_cmpint(rw_cli_status_from_failure(NULL), ==, 0);
}

#ifndef G_OS_WIN32
/* A fake CLI: a shell script that records how it was called. */
static gchar *
make_fake_cli(const gchar *behaviour)
{
    gchar *dir = g_dir_make_tmp("rw-fake-cli-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "railway", NULL);
    gchar *script = g_strdup_printf(
        "#!/bin/sh\n"
        "d=$(dirname \"$0\")\n"
        "printf '%%s\\n' \"$@\" > \"$d/args.txt\"\n"
        "printf '%%s' \"$RAILWAY_API_TOKEN\" > \"$d/env.txt\"\n"
        "cat > \"$d/stdin.txt\"\n"
        "%s\n", behaviour);

    g_assert_true(g_file_set_contents(path, script, -1, NULL));
    g_assert_cmpint(g_chmod(path, 0755), ==, 0);
    g_free(script);
    g_free(dir);
    return path;
}

static gchar *
read_beside(const gchar *program, const gchar *name)
{
    gchar *dir = g_path_get_dirname(program);
    gchar *file = g_build_filename(dir, name, NULL);
    gchar *text = NULL;

    g_file_get_contents(file, &text, NULL, NULL);
    g_free(file);
    g_free(dir);
    return text;
}

static void
remove_fake_cli(gchar *program)
{
    const gchar *names[] = { "railway", "args.txt", "env.txt", "stdin.txt", NULL };
    gchar *dir = g_path_get_dirname(program);

    for (guint i = 0; names[i] != NULL; i++) {
        gchar *file = g_build_filename(dir, names[i], NULL);

        g_unlink(file);
        g_free(file);
    }
    g_rmdir(dir);
    g_free(dir);
    g_free(program);
    rw_cli_set_program(NULL);
}

static void
test_cli_secrets_stay_off_argv(void)
{
    gchar *program = make_fake_cli("printf '{\"data\":{\"ok\":true}}'");
    RwClient *client;
    JsonBuilder *builder = json_builder_new();
    JsonNode *variables, *data;
    GError *error = NULL;
    gchar *args, *stdin_text, *env;

    g_unsetenv("RAILWAY_API_TOKEN");
    rw_cli_set_program(program);
    client = rw_client_new(NULL);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "value");
    json_builder_add_string_value(builder, "s3cret-value");
    json_builder_end_object(builder);
    variables = json_builder_get_root(builder);

    data = rw_client_execute(client, "mutation($value: String!) { x(v: $value) }",
                             variables, &error);
    g_assert_no_error(error);
    g_assert_nonnull(data);

    args = read_beside(program, "args.txt");
    stdin_text = read_beside(program, "stdin.txt");
    env = read_beside(program, "env.txt");
    g_assert_nonnull(strstr(args, "--variables"));
    g_assert_nonnull(strstr(args, "@-"));
    g_assert_null(strstr(args, "s3cret-value"));
    g_assert_nonnull(strstr(stdin_text, "s3cret-value"));
    g_assert_cmpstr(env, ==, "");

    g_free(args);
    g_free(stdin_text);
    g_free(env);
    json_node_unref(data);
    json_node_unref(variables);
    g_object_unref(builder);
    rw_client_free(client);
    remove_fake_cli(program);
}

static void
test_cli_token_goes_in_environment_only(void)
{
    gchar *program = make_fake_cli("printf '{\"data\":{\"ok\":true}}'");
    RwClient *client;
    JsonNode *data;
    GError *error = NULL;
    gchar *args, *env;

    rw_cli_set_program(program);
    client = rw_client_new("tok-123");
    data = rw_client_execute(client, "query { ok }", NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(data);

    args = read_beside(program, "args.txt");
    env = read_beside(program, "env.txt");
    g_assert_null(strstr(args, "tok-123"));
    g_assert_cmpstr(env, ==, "tok-123");

    g_free(args);
    g_free(env);
    json_node_unref(data);
    rw_client_free(client);
    remove_fake_cli(program);
}

static void
test_cli_http_error_with_body(void)
{
    gchar *program = make_fake_cli(
        "printf '{\"errors\":[{\"message\":\"Cannot query field\"}]}'\n"
        "echo 'Railway API request failed with HTTP 400 Bad Request' >&2\n"
        "exit 1");
    RwClient *client;
    GError *error = NULL;

    rw_cli_set_program(program);
    client = rw_client_new(NULL);
    g_assert_null(rw_client_execute(client, "query { x }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_GRAPHQL);
    g_assert_nonnull(strstr(error->message, "Cannot query field"));
    g_error_free(error);
    rw_client_free(client);
    remove_fake_cli(program);
}

static void
test_cli_signed_out(void)
{
    gchar *program = make_fake_cli(
        "echo 'Unauthorized. Please login with `railway login`.' >&2\n"
        "exit 1");
    RwClient *client;
    GError *error = NULL;

    rw_cli_set_program(program);
    client = rw_client_new(NULL);
    g_assert_null(rw_client_execute(client, "query { x }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_AUTH);
    g_error_free(error);
    rw_client_free(client);
    remove_fake_cli(program);
}

static void
test_cli_unknown_failure(void)
{
    gchar *program = make_fake_cli("echo 'boom' >&2\nexit 3");
    RwClient *client;
    GError *error = NULL;

    rw_cli_set_program(program);
    client = rw_client_new(NULL);
    rw_client_set_max_retries(client, 0);
    g_assert_null(rw_client_execute(client, "query { x }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_CLI);
    g_assert_nonnull(strstr(error->message, "boom"));
    g_error_free(error);
    rw_client_free(client);
    remove_fake_cli(program);
}

static void
test_cli_whoami(void)
{
    gchar *program = make_fake_cli(
        "printf '{\"name\":\"Ross\",\"email\":\"ross@example.test\",\"workspaces\":[]}'");
    gchar *name = NULL, *email = NULL;
    GError *error = NULL;

    rw_cli_set_program(program);
    g_assert_true(rw_cli_whoami(&name, &email, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(name, ==, "Ross");
    g_assert_cmpstr(email, ==, "ross@example.test");
    g_free(name);
    g_free(email);
    remove_fake_cli(program);

    program = make_fake_cli("echo 'Unauthorized. Please login with `railway login`.' >&2\nexit 1");
    rw_cli_set_program(program);
    g_assert_false(rw_cli_whoami(&name, &email, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_AUTH);
    g_error_free(error);
    remove_fake_cli(program);
}

static void
test_cli_version(void)
{
    gchar *program = make_fake_cli("echo 'railway 9.9.9'");
    GError *error = NULL;
    gchar *version;

    rw_cli_set_program(program);
    version = rw_cli_version(&error);
    g_assert_no_error(error);
    g_assert_cmpstr(version, ==, "railway 9.9.9");
    g_free(version);
    remove_fake_cli(program);
}
#endif

static void
test_cli_missing(void)
{
    RwClient *client;
    GError *error = NULL;

    rw_cli_set_program("/nonexistent/path/railway");
    client = rw_client_new(NULL);
    rw_client_set_max_retries(client, 0);
    g_assert_null(rw_client_execute(client, "query { x }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_CLI);
    g_error_free(error);
    rw_client_free(client);
    rw_cli_set_program(NULL);
}

static void
test_upsert_false_is_a_failure(void)
{
    Mock mock;
    Canned replies[] = { { 200, "{\"data\":{\"variableCollectionUpsert\":false}}", -1 } };
    RwClient *client = make_client(&mock, replies, 1);
    GHashTable *vars = rw_variables_new();
    GError *error = NULL;

    g_hash_table_insert(vars, g_strdup("A"), g_strdup("b"));
    g_assert_false(rw_upsert_variables(client, "p1", "e1", NULL, vars, TRUE, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_GRAPHQL);
    g_error_free(error);
    g_hash_table_unref(vars);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_redeploy_false_is_a_failure(void)
{
    Mock mock;
    Canned replies[] = {
        { 200, "{\"data\":{\"serviceInstanceRedeploy\":false}}", -1 },
        { 200, "{\"data\":{\"somethingElse\":true}}", -1 },
    };
    RwClient *client = make_client(&mock, replies, 2);
    GError *error = NULL;

    g_assert_false(rw_redeploy_service(client, "s1", "e1", &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_GRAPHQL);
    g_clear_error(&error);
    g_assert_false(rw_redeploy_service(client, "s1", "e1", &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_GRAPHQL);
    g_error_free(error);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_non_success_status_with_data_is_an_error(void)
{
    Mock mock;
    Canned replies[] = { { 400, "{\"data\":{\"ok\":true}}", -1 } };
    RwClient *client = make_client(&mock, replies, 1);
    GError *error = NULL;

    g_assert_null(rw_client_execute(client, "query { ok }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_HTTP);
    g_error_free(error);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
}

static void
test_missing_cli_is_not_retried(void)
{
    Mock mock = { NULL, 0, 0, NULL, 0 };
    RwClient *client;
    GError *error = NULL;

    mock.requests = g_ptr_array_new_with_free_func(g_free);
    rw_cli_set_program("/nonexistent/path/railway");
    client = rw_client_new(NULL);                      /* default retries: five */
    rw_client_set_sleep(client, mock_sleep, &mock);
    g_assert_null(rw_client_execute(client, "query { x }", NULL, &error));
    g_assert_error(error, RW_ERROR, RW_ERROR_CLI);
    g_assert_cmpuint(mock.slept, ==, 0);               /* no dead wait for a permanent error */
    g_assert_cmpuint(rw_client_get_request_count(client), ==, 1);
    g_error_free(error);
    g_ptr_array_unref(mock.requests);
    rw_client_free(client);
    rw_cli_set_program(NULL);
}

static void
test_secure_free(void)
{
    rw_secure_free(NULL);
    rw_secure_free(g_strdup("secret"));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/execute/returns-data", test_execute_returns_data);
    g_test_add_func("/execute/graphql-error", test_graphql_error);
    g_test_add_func("/execute/auth-errors", test_auth_errors);
    g_test_add_func("/execute/rate-limit-retry", test_rate_limit_retry);
    g_test_add_func("/execute/rate-limit-too-long", test_rate_limit_too_long);
    g_test_add_func("/execute/server-error-retried", test_server_error_retried);
    g_test_add_func("/list/workspaces", test_list_workspaces);
    g_test_add_func("/list/projects-pages", test_list_projects_pages);
    g_test_add_func("/list/paging-must-advance", test_paging_must_advance);
    g_test_add_func("/list/services-and-environments", test_list_services_and_environments);
    g_test_add_func("/variables/get", test_get_variables);
    g_test_add_func("/variables/upsert-never-replaces", test_upsert_never_replaces);
    g_test_add_func("/variables/upsert-shared-omits-service", test_upsert_shared_omits_service);
    g_test_add_func("/variables/upsert-rejects-empty", test_upsert_rejects_empty);
    g_test_add_func("/deploy/redeploy-service", test_redeploy_service);
    g_test_add_func("/execute/rate-limit-unknown-wait", test_rate_limit_unknown_wait);
    g_test_add_func("/cli/split-request", test_cli_split_request);
    g_test_add_func("/cli/status-from-failure", test_cli_status_from_failure);
    g_test_add_func("/cli/missing", test_cli_missing);
#ifndef G_OS_WIN32
    g_test_add_func("/cli/secrets-stay-off-argv", test_cli_secrets_stay_off_argv);
    g_test_add_func("/cli/token-in-environment-only", test_cli_token_goes_in_environment_only);
    g_test_add_func("/cli/http-error-with-body", test_cli_http_error_with_body);
    g_test_add_func("/cli/signed-out", test_cli_signed_out);
    g_test_add_func("/cli/unknown-failure", test_cli_unknown_failure);
    g_test_add_func("/cli/whoami", test_cli_whoami);
    g_test_add_func("/cli/version", test_cli_version);
#endif
    g_test_add_func("/review/upsert-false-is-a-failure", test_upsert_false_is_a_failure);
    g_test_add_func("/review/redeploy-false-is-a-failure", test_redeploy_false_is_a_failure);
    g_test_add_func("/review/non-success-status-with-data", test_non_success_status_with_data_is_an_error);
    g_test_add_func("/review/missing-cli-is-not-retried", test_missing_cli_is_not_retried);
    g_test_add_func("/secrets/secure-free", test_secure_free);

    return g_test_run();
}
