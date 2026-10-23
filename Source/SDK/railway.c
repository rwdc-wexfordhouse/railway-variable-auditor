/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "railway.h"
#include "railway-private.h"

#include <string.h>

#define RW_DEFAULT_ENDPOINT "https://backboard.railway.com/graphql/v2"
#define RW_PAGE_SIZE        100
#define RW_MAX_BACKOFF      30

struct _RwClient {
    gchar          *token;
    gchar          *endpoint;
    RwTransportFunc transport;
    gpointer        transport_data;
    GDestroyNotify  transport_destroy;
    RwSleepFunc     sleep;
    gpointer        sleep_data;
    guint           max_retries;
    guint           max_wait;
    guint64         request_count;
    gint            rate_remaining;
    gint            retry_after;
};

G_DEFINE_QUARK(rw-error-quark, rw_error)

/* Entities and secrets */

RwEntity *
rw_entity_new(const gchar *id, const gchar *name)
{
    RwEntity *entity = g_new0(RwEntity, 1);

    entity->id = g_strdup(id);
    entity->name = g_strdup(name != NULL ? name : "");
    return entity;
}

void
rw_entity_free(RwEntity *entity)
{
    if (entity == NULL)
        return;
    g_free(entity->id);
    g_free(entity->name);
    g_free(entity);
}

void
rw_secure_free(gchar *secret)
{
    if (secret == NULL)
        return;

    volatile gchar *p = secret;
    gsize n = strlen(secret);

    while (n-- > 0)
        *p++ = 0;
    g_free(secret);
}

static void
secret_destroy(gpointer data)
{
    rw_secure_free(data);
}

GHashTable *
rw_variables_new(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, secret_destroy);
}

/* Client */

static void
default_sleep(gpointer user_data, guint seconds)
{
    (void)user_data;
    g_usleep((gulong)seconds * G_USEC_PER_SEC);
}

RwClient *
rw_client_new(const gchar *token)
{
    RwClient *client = g_new0(RwClient, 1);

    client->token = (token != NULL && *token != '\0') ? g_strdup(token) : NULL;
    client->endpoint = g_strdup(RW_DEFAULT_ENDPOINT);
    client->transport = rw_cli_transport_send;
    client->sleep = default_sleep;
    client->max_retries = 5;
    client->max_wait = 120;
    client->rate_remaining = -1;
    client->retry_after = -1;
    return client;
}

void
rw_client_free(RwClient *client)
{
    if (client == NULL)
        return;
    if (client->transport_destroy != NULL)
        client->transport_destroy(client->transport_data);
    rw_secure_free(client->token);
    g_free(client->endpoint);
    g_free(client);
}

void
rw_client_set_endpoint(RwClient *client, const gchar *url)
{
    g_return_if_fail(client != NULL && url != NULL);
    g_free(client->endpoint);
    client->endpoint = g_strdup(url);
}

void
rw_client_set_transport(RwClient *client, RwTransportFunc func,
                        gpointer user_data, GDestroyNotify destroy)
{
    g_return_if_fail(client != NULL && func != NULL);
    if (client->transport_destroy != NULL)
        client->transport_destroy(client->transport_data);
    client->transport = func;
    client->transport_data = user_data;
    client->transport_destroy = destroy;
}

void
rw_client_set_sleep(RwClient *client, RwSleepFunc func, gpointer user_data)
{
    g_return_if_fail(client != NULL);
    client->sleep = func != NULL ? func : default_sleep;
    client->sleep_data = user_data;
}

void
rw_client_set_max_retries(RwClient *client, guint retries)
{
    g_return_if_fail(client != NULL);
    client->max_retries = retries;
}

void
rw_client_set_max_wait(RwClient *client, guint seconds)
{
    g_return_if_fail(client != NULL);
    client->max_wait = seconds;
}

guint64
rw_client_get_request_count(const RwClient *client)
{
    g_return_val_if_fail(client != NULL, 0);
    return client->request_count;
}

gint
rw_client_get_rate_remaining(const RwClient *client)
{
    g_return_val_if_fail(client != NULL, -1);
    return client->rate_remaining;
}

gint
rw_client_get_retry_after(const RwClient *client)
{
    g_return_val_if_fail(client != NULL, -1);
    return client->retry_after;
}

/* JSON helpers */

static JsonNode *
json_at(JsonNode *node, const gchar *const *path)
{
    for (; node != NULL && *path != NULL; path++) {
        if (!JSON_NODE_HOLDS_OBJECT(node))
            return NULL;
        node = json_object_get_member(json_node_get_object(node), *path);
    }
    return node;
}

static const gchar *
json_string_member(JsonObject *object, const gchar *member)
{
    JsonNode *node = json_object_get_member(object, member);

    if (node != NULL && JSON_NODE_HOLDS_VALUE(node)
        && json_node_get_value_type(node) == G_TYPE_STRING)
        return json_node_get_string(node);
    return NULL;
}

static gboolean
add_entity(JsonNode *node, GPtrArray *out)
{
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return FALSE;

    JsonObject *object = json_node_get_object(node);
    const gchar *id = json_string_member(object, "id");

    if (id == NULL)
        return FALSE;
    g_ptr_array_add(out, rw_entity_new(id, json_string_member(object, "name")));
    return TRUE;
}

/* GraphQL execution */

static gchar *
build_body(const gchar *query, JsonNode *variables)
{
    JsonBuilder *builder = json_builder_new();
    JsonGenerator *generator = json_generator_new();
    JsonNode *root;
    gchar *body;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "query");
    json_builder_add_string_value(builder, query);
    if (variables != NULL) {
        json_builder_set_member_name(builder, "variables");
        json_builder_add_value(builder, json_node_copy(variables));
    }
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    body = json_generator_to_data(generator, NULL);

    json_node_unref(root);
    g_object_unref(generator);
    g_object_unref(builder);
    return body;
}

static void
response_clear(RwHttpResponse *response)
{
    rw_secure_free(response->body);
    response->body = NULL;
}

static guint
backoff_seconds(guint attempt)
{
    return attempt >= 5 ? RW_MAX_BACKOFF : MIN(1u << attempt, (guint)RW_MAX_BACKOFF);
}

/* Returns FALSE and sets error when the document carries GraphQL errors. */
static gboolean
check_graphql_errors(JsonNode *root, GError **error)
{
    JsonObject *object = json_node_get_object(root);
    JsonNode *errors = json_object_get_member(object, "errors");

    if (errors == NULL || !JSON_NODE_HOLDS_ARRAY(errors)
        || json_array_get_length(json_node_get_array(errors)) == 0)
        return TRUE;

    JsonNode *first = json_array_get_element(json_node_get_array(errors), 0);
    const gchar *message = NULL;

    if (first != NULL && JSON_NODE_HOLDS_OBJECT(first))
        message = json_string_member(json_node_get_object(first), "message");
    if (message == NULL)
        message = "Unknown error";

    gboolean auth = strstr(message, "Not Authorized") != NULL
                 || strstr(message, "Unauthorized") != NULL;

    g_set_error(error, RW_ERROR, auth ? RW_ERROR_AUTH : RW_ERROR_GRAPHQL,
                "Railway: %s", message);
    return FALSE;
}

JsonNode *
rw_client_execute(RwClient *client, const gchar *query, JsonNode *variables,
                  GError **error)
{
    g_return_val_if_fail(client != NULL && query != NULL, NULL);

    gchar *body = build_body(query, variables);
    JsonNode *result = NULL;

    for (guint attempt = 0;; attempt++) {
        RwHttpResponse response = { 0, NULL, -1, -1, -1 };
        GError *local = NULL;

        client->request_count++;
        if (!client->transport(client->transport_data, client->endpoint,
                               client->token, body, &response, &local)) {
            response_clear(&response);
            if (attempt < client->max_retries
                && !(local->domain == RW_ERROR
                     && (local->code == RW_ERROR_CLI || local->code == RW_ERROR_INVALID))) {
                g_clear_error(&local);
                client->sleep(client->sleep_data, backoff_seconds(attempt));
                continue;
            }
            g_propagate_error(error, local);
            break;
        }

        if (response.rate_remaining >= 0)
            client->rate_remaining = response.rate_remaining;
        client->retry_after = response.retry_after;

        if (response.status == 429) {
            gboolean known = response.retry_after > 0;
            guint wait = known ? (guint)response.retry_after : backoff_seconds(attempt);

            response_clear(&response);
            if (wait > client->max_wait || attempt >= client->max_retries) {
                if (known)
                    g_set_error(error, RW_ERROR, RW_ERROR_RATE_LIMITED,
                                "Railway rate limit reached. Try again in %u seconds.",
                                wait);
                else
                    g_set_error_literal(error, RW_ERROR, RW_ERROR_RATE_LIMITED,
                                        "Railway rate limit reached and Railway did not "
                                        "say how long to wait. Wait a while, then resume.");
                break;
            }
            client->sleep(client->sleep_data, wait);
            continue;
        }

        if (response.status >= 500 && response.status < 600) {
            gint status = response.status;

            response_clear(&response);
            if (attempt < client->max_retries) {
                client->sleep(client->sleep_data, backoff_seconds(attempt));
                continue;
            }
            g_set_error(error, RW_ERROR, RW_ERROR_HTTP,
                        "Railway returned HTTP %d.", status);
            break;
        }

        if (response.status == 401 || response.status == 403) {
            response_clear(&response);
            g_set_error_literal(error, RW_ERROR, RW_ERROR_AUTH,
                                "Railway rejected the credentials. "
                                "Sign in again with railway login.");
            break;
        }

        JsonParser *parser = json_parser_new();
        GError *parse_error = NULL;
        gboolean parsed = response.body != NULL
            && json_parser_load_from_data(parser, response.body, -1, &parse_error);
        JsonNode *root = parsed ? json_parser_get_root(parser) : NULL;
        gint status = response.status;

        response_clear(&response);
        g_clear_error(&parse_error);

        if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
            if (status < 200 || status >= 300)
                g_set_error(error, RW_ERROR, RW_ERROR_HTTP,
                            "Railway returned HTTP %d.", status);
            else
                g_set_error_literal(error, RW_ERROR, RW_ERROR_PARSE,
                                    "Railway sent a response that is not JSON.");
        } else if (check_graphql_errors(root, error)) {
            JsonNode *data = json_object_get_member(json_node_get_object(root), "data");

            if (status < 200 || status >= 300)
                g_set_error(error, RW_ERROR, RW_ERROR_HTTP,
                            "Railway returned HTTP %d.", status);
            else if (data == NULL || JSON_NODE_HOLDS_NULL(data))
                g_set_error_literal(error, RW_ERROR, RW_ERROR_PARSE,
                                    "Railway sent a response without data.");
            else
                result = json_node_copy(data);
        }
        g_object_unref(parser);
        break;
    }

    rw_secure_free(body);
    return result;
}

/* Listing */

static GPtrArray *
new_entities(void)
{
    return g_ptr_array_new_with_free_func((GDestroyNotify)rw_entity_free);
}

static GPtrArray *
fetch_connection(RwClient *client, const gchar *query, const gchar *id_name,
                 const gchar *id_value, const gchar *const *path, GError **error)
{
    GPtrArray *entities = new_entities();
    gchar *after = NULL;

    for (;;) {
        JsonBuilder *builder = json_builder_new();

        json_builder_begin_object(builder);
        if (id_name != NULL) {
            json_builder_set_member_name(builder, id_name);
            json_builder_add_string_value(builder, id_value);
        }
        json_builder_set_member_name(builder, "first");
        json_builder_add_int_value(builder, RW_PAGE_SIZE);
        if (after != NULL) {
            json_builder_set_member_name(builder, "after");
            json_builder_add_string_value(builder, after);
        }
        json_builder_end_object(builder);

        JsonNode *variables = json_builder_get_root(builder);
        JsonNode *data = rw_client_execute(client, query, variables, error);

        json_node_unref(variables);
        g_object_unref(builder);
        if (data == NULL)
            goto fail;

        JsonNode *connection = json_at(data, path);
        JsonNode *edges = connection != NULL && JSON_NODE_HOLDS_OBJECT(connection)
            ? json_object_get_member(json_node_get_object(connection), "edges") : NULL;

        if (edges == NULL || !JSON_NODE_HOLDS_ARRAY(edges)) {
            g_set_error_literal(error, RW_ERROR, RW_ERROR_PARSE,
                                "Railway sent a list in an unexpected shape.");
            json_node_unref(data);
            goto fail;
        }

        JsonArray *array = json_node_get_array(edges);

        for (guint i = 0; i < json_array_get_length(array); i++) {
            JsonNode *edge = json_array_get_element(array, i);

            if (JSON_NODE_HOLDS_OBJECT(edge))
                add_entity(json_object_get_member(json_node_get_object(edge), "node"),
                           entities);
        }

        JsonNode *info = json_object_get_member(json_node_get_object(connection), "pageInfo");
        gboolean more = FALSE;
        gchar *cursor = NULL;

        if (info != NULL && JSON_NODE_HOLDS_OBJECT(info)) {
            JsonObject *info_object = json_node_get_object(info);
            JsonNode *flag = json_object_get_member(info_object, "hasNextPage");

            more = flag != NULL && JSON_NODE_HOLDS_VALUE(flag)
                && json_node_get_boolean(flag);
            cursor = g_strdup(json_string_member(info_object, "endCursor"));
        }
        json_node_unref(data);

        if (!more) {
            g_free(cursor);
            break;
        }
        if (cursor == NULL || g_strcmp0(cursor, after) == 0) {
            g_free(cursor);
            g_set_error_literal(error, RW_ERROR, RW_ERROR_PARSE,
                                "Railway paging did not advance.");
            goto fail;
        }
        g_free(after);
        after = cursor;
    }

    g_free(after);
    return entities;

fail:
    g_free(after);
    g_ptr_array_unref(entities);
    return NULL;
}

GPtrArray *
rw_list_workspaces(RwClient *client, GError **error)
{
    static const gchar *const path[] = { "me", "workspaces", NULL };
    JsonNode *data = rw_client_execute(client,
        "query { me { workspaces { id name } } }", NULL, error);

    if (data == NULL)
        return NULL;

    JsonNode *list = json_at(data, path);
    GPtrArray *entities = new_entities();

    if (list == NULL || !JSON_NODE_HOLDS_ARRAY(list)) {
        g_set_error_literal(error, RW_ERROR, RW_ERROR_PARSE,
                            "Railway sent the workspace list in an unexpected shape.");
        g_ptr_array_unref(entities);
        json_node_unref(data);
        return NULL;
    }

    JsonArray *array = json_node_get_array(list);

    for (guint i = 0; i < json_array_get_length(array); i++)
        add_entity(json_array_get_element(array, i), entities);
    json_node_unref(data);
    return entities;
}

GPtrArray *
rw_list_projects(RwClient *client, const gchar *workspace_id, GError **error)
{
    static const gchar *const path[] = { "projects", NULL };

    g_return_val_if_fail(client != NULL && workspace_id != NULL, NULL);

    return fetch_connection(client,
        "query projects($workspaceId: String!, $first: Int, $after: String) {"
        " projects(workspaceId: $workspaceId, first: $first, after: $after) {"
        " edges { node { id name } } pageInfo { hasNextPage endCursor } } }",
        "workspaceId", workspace_id, path, error);
}

GPtrArray *
rw_list_services(RwClient *client, const gchar *project_id, GError **error)
{
    static const gchar *const path[] = { "project", "services", NULL };

    g_return_val_if_fail(client != NULL && project_id != NULL, NULL);
    return fetch_connection(client,
        "query services($projectId: String!, $first: Int, $after: String) {"
        " project(id: $projectId) { services(first: $first, after: $after) {"
        " edges { node { id name } } pageInfo { hasNextPage endCursor } } } }",
        "projectId", project_id, path, error);
}

GPtrArray *
rw_list_environments(RwClient *client, const gchar *project_id, GError **error)
{
    static const gchar *const path[] = { "project", "environments", NULL };

    g_return_val_if_fail(client != NULL && project_id != NULL, NULL);
    return fetch_connection(client,
        "query environments($projectId: String!, $first: Int, $after: String) {"
        " project(id: $projectId) { environments(first: $first, after: $after) {"
        " edges { node { id name } } pageInfo { hasNextPage endCursor } } } }",
        "projectId", project_id, path, error);
}

/* Variables */

GHashTable *
rw_get_variables(RwClient *client, const gchar *project_id,
                 const gchar *environment_id, const gchar *service_id,
                 gboolean unrendered, GError **error)
{
    static const gchar *const path[] = { "variables", NULL };

    g_return_val_if_fail(client != NULL && project_id != NULL
                         && environment_id != NULL, NULL);

    JsonBuilder *builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "projectId");
    json_builder_add_string_value(builder, project_id);
    json_builder_set_member_name(builder, "environmentId");
    json_builder_add_string_value(builder, environment_id);
    if (service_id != NULL) {
        json_builder_set_member_name(builder, "serviceId");
        json_builder_add_string_value(builder, service_id);
    }
    json_builder_set_member_name(builder, "unrendered");
    json_builder_add_boolean_value(builder, unrendered);
    json_builder_end_object(builder);

    JsonNode *variables = json_builder_get_root(builder);
    JsonNode *data = rw_client_execute(client,
        "query variables($projectId: String!, $environmentId: String!,"
        " $serviceId: String, $unrendered: Boolean) {"
        " variables(projectId: $projectId, environmentId: $environmentId,"
        " serviceId: $serviceId, unrendered: $unrendered) }",
        variables, error);

    json_node_unref(variables);
    g_object_unref(builder);
    if (data == NULL)
        return NULL;

    JsonNode *node = json_at(data, path);

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) {
        g_set_error_literal(error, RW_ERROR, RW_ERROR_PARSE,
                            "Railway sent variables in an unexpected shape.");
        json_node_unref(data);
        return NULL;
    }

    GHashTable *table = rw_variables_new();
    JsonObject *object = json_node_get_object(node);
    GList *names = json_object_get_members(object);

    for (GList *l = names; l != NULL; l = l->next) {
        const gchar *name = l->data;
        const gchar *value = json_string_member(object, name);

        g_hash_table_replace(table, g_strdup(name), g_strdup(value != NULL ? value : ""));
    }
    g_list_free(names);
    json_node_unref(data);
    return table;
}

/* A mutation answers true when Railway accepted it. Anything else is a failure. */
static gboolean
accepted(JsonNode *data, const gchar *member, GError **error)
{
    JsonNode *flag;
    gboolean ok;

    if (data == NULL)
        return FALSE;
    flag = JSON_NODE_HOLDS_OBJECT(data)
               ? json_object_get_member(json_node_get_object(data), member) : NULL;
    ok = flag != NULL && JSON_NODE_HOLDS_VALUE(flag)
         && json_node_get_value_type(flag) == G_TYPE_BOOLEAN && json_node_get_boolean(flag);
    json_node_unref(data);
    if (!ok)
        g_set_error(error, RW_ERROR, RW_ERROR_GRAPHQL,
                    "Railway did not accept the change (%s did not return true).", member);
    return ok;
}

gboolean
rw_upsert_variables(RwClient *client, const gchar *project_id,
                    const gchar *environment_id, const gchar *service_id,
                    GHashTable *variables, gboolean skip_deploys, GError **error)
{
    g_return_val_if_fail(client != NULL && project_id != NULL
                         && environment_id != NULL, FALSE);

    if (variables == NULL || g_hash_table_size(variables) == 0) {
        g_set_error_literal(error, RW_ERROR, RW_ERROR_INVALID,
                            "No variables to write.");
        return FALSE;
    }

    JsonBuilder *builder = json_builder_new();
    GHashTableIter iter;
    gpointer key, value;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "input");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "projectId");
    json_builder_add_string_value(builder, project_id);
    json_builder_set_member_name(builder, "environmentId");
    json_builder_add_string_value(builder, environment_id);
    if (service_id != NULL) {
        json_builder_set_member_name(builder, "serviceId");
        json_builder_add_string_value(builder, service_id);
    }
    json_builder_set_member_name(builder, "variables");
    json_builder_begin_object(builder);
    g_hash_table_iter_init(&iter, variables);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        json_builder_set_member_name(builder, key);
        json_builder_add_string_value(builder, value);
    }
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "replace");
    json_builder_add_boolean_value(builder, FALSE);
    json_builder_set_member_name(builder, "skipDeploys");
    json_builder_add_boolean_value(builder, skip_deploys);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    JsonNode *input = json_builder_get_root(builder);
    JsonNode *data = rw_client_execute(client,
        "mutation variableCollectionUpsert($input: VariableCollectionUpsertInput!) {"
        " variableCollectionUpsert(input: $input) }",
        input, error);

    json_node_unref(input);
    g_object_unref(builder);
    return accepted(data, "variableCollectionUpsert", error);
}

gboolean
rw_redeploy_service(RwClient *client, const gchar *service_id,
                  const gchar *environment_id, GError **error)
{
    g_return_val_if_fail(client != NULL && service_id != NULL
                         && environment_id != NULL, FALSE);

    JsonBuilder *builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "serviceId");
    json_builder_add_string_value(builder, service_id);
    json_builder_set_member_name(builder, "environmentId");
    json_builder_add_string_value(builder, environment_id);
    json_builder_end_object(builder);

    JsonNode *variables = json_builder_get_root(builder);
    JsonNode *data = rw_client_execute(client,
        "mutation serviceInstanceRedeploy($serviceId: String!, $environmentId: String!) {"
        " serviceInstanceRedeploy(serviceId: $serviceId, environmentId: $environmentId) }",
        variables, error);

    json_node_unref(variables);
    g_object_unref(builder);
    return accepted(data, "serviceInstanceRedeploy", error);
}
