/*
 * Railway Variable Auditor engine (rvcore)
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 *
 * Scan every workspace, classify every variable, plan a rotation of the
 * shared secrets, stage it, deploy it, and undo it. Built on the Railway SDK
 * (railway.h). Every step that matters is stored in a run folder on disk, so a
 * run can be resumed or undone later, and the whole job can be repeated the
 * next time an incident happens.
 *
 * Calls block, so run them off the GUI thread.
 */
#ifndef RVCORE_H
#define RVCORE_H

#include <gio/gio.h>
#include <glib.h>

#include "railway.h"

G_BEGIN_DECLS

#define RV_ERROR (rv_error_quark())
GQuark rv_error_quark(void);

typedef enum {
    RV_ERROR_INVALID,    /* bad arguments or an unsupported value format */
    RV_ERROR_IO,         /* a file could not be read or written */
    RV_ERROR_CSV,        /* a CSV file is malformed */
    RV_ERROR_CANCELLED,  /* the user cancelled */
    RV_ERROR_RANDOM,     /* no secure random source */
    RV_ERROR_STATE,      /* the step cannot run now, for example an incomplete scan */
    RV_ERROR_STOPPED,    /* a step stopped part way. The run folder allows resume or undo */
    RV_ERROR_DB,         /* the database could not be reached, or refused a statement */
    RV_ERROR_DB_AUTH,    /* the database refused the login */
    RV_ERROR_TUNNEL      /* the secure tunnel to a database did not open */
} RvErrorCode;

/* Groups. The order of checks is: INI, database credential, internal
 * hostname, reference, then the shared and unshared tests. */
typedef enum {
    RV_CAT_UNCLASSIFIED,
    RV_CAT_ROTATE,         /* "shared": a shared secret the tool will change */
    RV_CAT_DB_ROLE,        /* "database-role": the password of a database role, changed with the role */
    RV_CAT_SHARED_MANUAL,  /* shared secret the tool cannot safely change */
    RV_CAT_SHARED_CONFIG,  /* shared, but not a secret */
    RV_CAT_INI,            /* "ini-config" */
    RV_CAT_DATABASE,       /* "database-credential" */
    RV_CAT_HOSTNAME,       /* "internal-hostname" */
    RV_CAT_REFERENCE,      /* value is only a ${{...}} reference */
    RV_CAT_UNSHARED        /* "unshared" */
} RvCategory;

const gchar *rv_category_name(RvCategory category);

/* Progress. `fraction` is 0 to 1, or negative when unknown. */
typedef void (*RvProgressFunc)(gpointer user_data, const gchar *message, gdouble fraction);

/* ---- Random, generation ------------------------------------------------ */

gboolean rv_random_bytes(guint8 *buffer, gsize length, GError **error);
/* Unbiased. `alphabet` has at most 256 characters. Free with rw_secure_free(). */
gchar   *rv_random_string(const gchar *alphabet, gsize length, GError **error);

gboolean rv_name_is_secretish(const gchar *name);
gboolean rv_name_is_internal_secret(const gchar *name);
gboolean rv_name_is_provider(const gchar *name);
gboolean rv_value_has_provider_prefix(const gchar *value);
/* Long, single token, high entropy, or a secret-like name. */
gboolean rv_looks_like_secret(const gchar *name, const gchar *value);
/* Can rv_generate_like() make a replacement in the same format? */
gboolean rv_format_supported(const gchar *value);
/*
 * A new random value in the same format and length as `old_value`: UUID, hex,
 * digits, base64, base64url or alphanumeric. Fails with RV_ERROR_INVALID for
 * anything else. Free with rw_secure_free().
 */
gchar   *rv_generate_like(const gchar *old_value, GError **error);

/*
 * A PostgreSQL SCRAM-SHA-256 verifier for `password`, with a random salt:
 * "SCRAM-SHA-256$4096:<salt>$<StoredKey>:<ServerKey>". Sending this in
 * ALTER ROLE keeps the plaintext password out of the server's statement log.
 * Fails with RV_ERROR_INVALID for an empty password or one with characters
 * other than plain ASCII. Free with g_free().
 */
gchar   *rv_scram_verifier(const gchar *password, GError **error);
/* Does `password` match `verifier`? */
gboolean rv_scram_verify(const gchar *password, const gchar *verifier);

/* ---- PostgreSQL connection URLs ------------------------------------------------ */

typedef struct {
    gchar *user;
    gchar *password;     /* a secret, wiped when freed */
    gchar *host;
    guint  port;         /* 5432 if the URL has none */
    gchar *database;     /* NULL if the URL has none */
} RvPgUrl;

/* postgres:// or postgresql:// with a user and a password, or NULL. Percent-decoded. */
RvPgUrl *rv_pgurl_parse(const gchar *value);
void     rv_pgurl_free(RvPgUrl *url);
/*
 * Replace every occurrence of `old_password` in `value`, written raw or
 * percent-encoded, with `new_password`. NULL if it does not occur. The new
 * password should be letters and digits, so it needs no encoding.
 */
gchar   *rv_pgurl_replace_password(const gchar *value, const gchar *old_password,
                                   const gchar *new_password);

/* ---- Policy ------------------------------------------------------------ */

typedef struct _RvVariable RvVariable;
typedef struct _RvPolicy RvPolicy;

RvPolicy *rv_policy_new(void);
void      rv_policy_free(RvPolicy *policy);
gchar    *rv_policy_default_path(void);           /* free with g_free() */
/* A missing file gives an empty policy. */
RvPolicy *rv_policy_load(const gchar *path, GError **error);
gboolean  rv_policy_save(const RvPolicy *policy, const gchar *path, GError **error);

gboolean rv_policy_workspace_skipped(const RvPolicy *policy, const gchar *workspace_id);
void     rv_policy_skip_workspace(RvPolicy *policy, const gchar *workspace_id, gboolean skip);
gchar   *rv_policy_variable_key(const RvVariable *variable);  /* project/environment/service/NAME */
gboolean rv_policy_is_pinned(const RvPolicy *policy, const RvVariable *variable);
void     rv_policy_pin(RvPolicy *policy, const RvVariable *variable, gboolean pin);
void     rv_policy_add_credential_name(RvPolicy *policy, const gchar *name);
void     rv_policy_add_internal_name(RvPolicy *policy, const gchar *name);
gboolean rv_policy_is_credential_name(const RvPolicy *policy, const gchar *name);
gboolean rv_policy_is_internal_name(const RvPolicy *policy, const gchar *name);

/* ---- Inventory --------------------------------------------------------- */

struct _RvVariable {
    gchar *workspace_id, *workspace;
    gchar *project_id, *project;
    gchar *environment_id, *environment;
    gchar *service_id;     /* NULL for an environment level shared variable */
    gchar *service;        /* the service name, or "shared" */
    gchar *name;
    gchar *value;          /* a secret. Wiped when the variable is freed */
    RvCategory category;
    gchar *reason;
    gint   group;          /* rotation group index, or -1 */
};

RvVariable *rv_variable_new(const gchar *workspace_id, const gchar *workspace,
                            const gchar *project_id, const gchar *project,
                            const gchar *environment_id, const gchar *environment,
                            const gchar *service_id, const gchar *service,
                            const gchar *name, const gchar *value);
void        rv_variable_free(RvVariable *variable);

typedef struct {
    gchar *workspace;
    gchar *project;    /* NULL on a workspace row */
    gchar *status;     /* "done", "skipped" or "failed" */
    gchar *detail;
} RvReportRow;

/*
 * One role of one Postgres service, and every variable that holds its password.
 * Only roles that can be rotated safely are listed (see rv_classify).
 */
typedef struct {
    gchar *project_id, *project;
    gchar *environment_id, *environment;
    gchar *db_service_id, *db_service;
    gchar *role;
    gchar *database;        /* from the connection URL, may be NULL */
    gchar *host;            /* the host in the URL, for the report */
    guint  port;
    gchar *old_password;    /* a secret, wiped when freed */
    GPtrArray *consumers;   /* RvVariable*, owned by the inventory */
} RvRole;

typedef struct {
    GPtrArray *variables;   /* RvVariable* */
    GPtrArray *report;      /* RvReportRow* */
    GPtrArray *roles;       /* RvRole*, safe to rotate */
    guint      workspaces, projects, environments, services;
    guint      rotation_groups;
    gboolean   complete;    /* FALSE if anything was skipped or failed */
} RvInventory;

RvInventory *rv_inventory_new(void);
void         rv_inventory_free(RvInventory *inventory);
void         rv_inventory_add_report(RvInventory *inventory, const gchar *workspace,
                                     const gchar *project, const gchar *status,
                                     const gchar *detail);

/* Sets category, reason and group on every variable. `policy` may be NULL. */
void rv_classify(RvInventory *inventory, const RvPolicy *policy);

/* ---- Scan -------------------------------------------------------------- */

typedef struct {
    RwClient       *client;
    const RvPolicy *policy;          /* may be NULL */
    RvProgressFunc  progress;        /* may be NULL */
    gpointer        progress_data;
    GCancellable   *cancel;          /* may be NULL */
} RvScanOptions;

typedef struct {
    gchar *workspace_id, *workspace, *id, *name;
    GPtrArray *environments;         /* RwEntity* */
    GPtrArray *services;             /* RwEntity* */
} RvProject;

typedef struct {
    GPtrArray *projects;             /* RvProject* */
    GPtrArray *report;               /* RvReportRow*, workspace rows and failures */
    guint      workspaces;
} RvTopology;

/* Step one: list workspaces, projects, environments and services. */
RvTopology *rv_discover(const RvScanOptions *options, GError **error);
/* Railway requests that reading the variables will need. */
guint       rv_topology_estimate(const RvTopology *topology);
/* Step two: read every variable, unrendered. Copies the report of `topology`. */
RvInventory *rv_read_variables(RvTopology *topology, const RvScanOptions *options,
                               GError **error);
void         rv_topology_free(RvTopology *topology);

/* ---- Plan -------------------------------------------------------------- */

typedef struct {
    gchar     *label;
    GPtrArray *members;      /* RvVariable*, owned by the inventory */
    gboolean   enabled;
    gchar     *new_value;    /* a secret */
} RvPlanGroup;

/* A database role to rotate, with its new password and the new value of each consumer. */
typedef struct {
    gchar        *label;
    const RvRole *role;        /* owned by the inventory */
    gboolean      enabled;
    gchar        *new_password; /* a secret */
    GPtrArray    *new_values;   /* gchar*, one per role->consumers, secrets */
} RvPlanRole;

typedef struct {
    RvInventory *inventory;  /* borrowed. Must outlive the plan */
    GPtrArray   *groups;     /* RvPlanGroup* */
    GPtrArray   *roles;      /* RvPlanRole* */
} RvPlan;

/*
 * Makes one new value for every rotation group, and one new password for every database role. Fails with RV_ERROR_STATE if
 * the scan was incomplete, unless `accept_incomplete` is set: a secret shared
 * with a place we could not read would be changed on one side only.
 */
RvPlan *rv_plan_build(RvInventory *inventory, gboolean accept_incomplete, GError **error);
void    rv_plan_free(RvPlan *plan);
/* Requests that staging (a read and a write for each batch) and deploying the enabled groups will need. */
guint   rv_plan_request_estimate(const RvPlan *plan);

/* ---- Run: write, stage, deploy ------------------------------------------ */

/*
 * The database and the tunnel to it sit behind these two small interfaces, so
 * the tests use fakes. NULL in RvRunOptions means the real ones: libpq, loaded
 * at run time, and `railway connect <service> --tunnel-only`.
 */
typedef struct {
    /* TRUE if the login works. FALSE with RV_ERROR_DB_AUTH if the server refuses it,
     * or RV_ERROR_DB for anything else (no connection, no such database). */
    gboolean (*login_ok)(gpointer data, const gchar *host, guint port, const gchar *database,
                         const gchar *user, const gchar *password, GError **error);
    /* ALTER ROLE <role> PASSWORD '<verifier>', run as the admin. The verifier is a
     * SCRAM-SHA-256 verifier, never the plain password. */
    gboolean (*set_password)(gpointer data, const gchar *host, guint port, const gchar *database,
                             const gchar *admin_user, const gchar *admin_password,
                             const gchar *role, const gchar *verifier, GError **error);
} RvDbOps;

typedef struct {
    /* Opens a local port that leads to the database service. The handle goes to close(). */
    gboolean (*open)(gpointer data, const gchar *project_id, const gchar *environment_id,
                     const gchar *service, guint *port, gpointer *handle, GError **error);
    void     (*close)(gpointer data, gpointer handle);
} RvTunnelOps;

const RvDbOps     *rv_db_ops_libpq(void);      /* the real database interface */
const RvTunnelOps *rv_tunnel_ops_cli(void);    /* the real tunnel, through the Railway CLI */
/* Is libpq available on this machine? */
gboolean           rv_libpq_available(GError **error);

typedef struct {
    RwClient       *client;
    RvProgressFunc  progress;
    gpointer        progress_data;
    GCancellable   *cancel;
    const RvDbOps     *db;             /* NULL for the real one */
    gpointer           db_data;
    const RvTunnelOps *tunnels;        /* NULL for the real one */
    gpointer           tunnel_data;
} RvRunOptions;

typedef struct {
    guint    roles_total, roles_done, roles_failed;
    guint    batches_total, batches_done;
    guint    deploys_total, deploys_done, deploys_failed;
    gboolean stage_failed;
} RvRunStatus;

/* Creates <base>/run-YYYYMMDD-HHMMSS (UTC), owner only. Free with g_free(). */
gchar   *rv_run_dir_new(const gchar *base, GError **error);
/*
 * Writes changes.csv and roles.csv (owner only, old and new values and passwords),
 * not-changed.csv, run-report.csv, deploys.csv and progress.csv. Nothing is sent to Railway.
 * Groups the user switched off are listed in not-changed.csv as shared-manual.
 */
gboolean rv_run_prepare(const RvPlan *plan, const gchar *run_dir, GError **error);
/*
 * Writes the new values with redeploys suppressed. Stops at the first failure.
 * Before each write it reads the current values and stops if a variable was
 * changed by someone since the scan, so their change is never overwritten.
 */
gboolean rv_run_stage(const gchar *run_dir, const RvRunOptions *options, GError **error);
/*
 * Changes the password of each database role in roles.csv, through a secure
 * tunnel, after the variables have been staged. For each role it first logs in
 * with the current password (proving the mapping is right), then changes it,
 * then logs in with the new one, and puts the old one back if that fails. Stops
 * at the first failure. Safe to repeat: a role that already has its new password
 * is recorded as done. Deploying is refused until every role is done.
 */
gboolean rv_run_roles(const gchar *run_dir, const RvRunOptions *options, GError **error);
/* Redeploys each affected service once. Carries on after a failure. */
gboolean rv_run_deploy(const gchar *run_dir, const RvRunOptions *options, GError **error);
gboolean rv_run_status(const gchar *run_dir, RvRunStatus *status, GError **error);

/* ---- Undo --------------------------------------------------------------- */

typedef struct {
    gboolean    dry_run;
    gboolean    overwrite_conflicts;
    gboolean    deploy;           /* redeploy the restored services */
    GHashTable *only_projects;    /* project ids (a set), NULL for all */
} RvUndoOptions;

typedef struct {
    guint restored, conflicts, already_original, skipped, failed;
    guint redeploy_failed;
} RvUndoResult;

/*
 * Restores the old values from changes.csv. A value is restored only if it
 * still holds the recorded new value. Writes undo-report.csv (or
 * undo-preview.csv for a dry run), even if cancelled part way. Safe to repeat.
 * With `deploy`, services are redeployed in every environment that holds the
 * old values, once, so a repeated undo finishes what an earlier one did not.
 */
gboolean rv_undo_run(const gchar *run_dir, const RvRunOptions *options,
                     const RvUndoOptions *undo, RvUndoResult *result, GError **error);

/* ---- Reports and files -------------------------------------------------- */

/* `include_rotatable` also lists the shared secrets the tool could rotate (category "shared"). */
gboolean rv_write_not_changed(const RvInventory *inventory, const gchar *path,
                              gboolean include_rotatable, GError **error);
gboolean rv_write_run_report(const RvInventory *inventory, const gchar *path, GError **error);
/*
 * Every variable with its CURRENT value (unrendered, so a reference is shown as
 * written), its group and the reason. Columns: workspace, project, environment,
 * service, variable, value, category, reason. This file holds live secrets, so it
 * is owner only. For an audit: nothing is rotated.
 */
gboolean rv_write_audit(const RvInventory *inventory, const gchar *path, GError **error);
/* The Desktop, or the home folder if there is none. Free with g_free(). */
gchar   *rv_audit_default_folder(void);
/* Creates a run folder in base and writes audit.csv into it. Returns the folder (free it),
 * or NULL with error set. */
gchar   *rv_audit_save(const RvInventory *inventory, const gchar *base, GError **error);

/* CSV helpers. Rows from rv_csv_parse are NULL-terminated vectors (g_strfreev). */
void       rv_csv_append_row(GString *out, const gchar *const *fields);
GPtrArray *rv_csv_parse(const gchar *text, GError **error);
/* Atomic. A private file is owner only (0600, or a restricted ACL on Windows). */
gboolean   rv_write_file(const gchar *path, const gchar *data, gsize length,
                         gboolean private_file, GError **error);

/* Current time as ISO 8601 in UTC. Free with g_free(). */
gchar *rv_now_iso(void);

G_END_DECLS

#endif /* RVCORE_H */
