/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#ifndef RV_PRIVATE_H
#define RV_PRIVATE_H

#include "rvcore.h"

G_BEGIN_DECLS

#define RV_FILE_CHANGES      "changes.csv"
#define RV_FILE_NOT_CHANGED  "not-changed.csv"
#define RV_FILE_RUN_REPORT   "run-report.csv"
#define RV_FILE_DEPLOYS      "deploys.csv"
#define RV_FILE_ROLES        "roles.csv"
#define RV_FILE_PROGRESS     "progress.csv"
#define RV_FILE_UNDO_REPORT  "undo-report.csv"
#define RV_FILE_UNDO_PREVIEW "undo-preview.csv"

/* A variable that the plan will change, with its new value. */
typedef struct {
    RvVariable  *variable;
    const gchar *new_value;
} RvChange;

typedef struct {
    gchar *project_id, *project;
    gchar *environment_id, *environment;
    gchar *service_id, *service;
} RvDeployTarget;

/* Rows of changes.csv grouped by project|environment|service. */
typedef struct {
    gchar *key, *project_id, *environment_id, *service_id, *label;
    GPtrArray *rows;      /* gchar** rows of changes.csv, borrowed from the caller */
} RvBatch;
GPtrArray *rv_batches_from_changes(GPtrArray *rows);   /* RvBatch*, skips the header row */

/* Roles: change and restore passwords (rv-roles-run.c). */
typedef struct {
    gchar *project_id, *environment_id, *project, *environment, *service, *role;
    const gchar *status;     /* restored, conflict, already-original, skipped or failed */
    gchar *detail;
} RvRoleOutcome;
void       rv_role_outcome_free(RvRoleOutcome *outcome);
GPtrArray *rv_roles_undo(const gchar *run_dir, const RvRunOptions *options,
                         const RvUndoOptions *undo, GError **error);   /* RvRoleOutcome* */
void       rv_roles_status(const gchar *run_dir, guint *total, guint *done, guint *failed);
gboolean   rv_roles_pending(const gchar *run_dir);

void rv_role_free(RvRole *role);
/* Called by rv_classify after the single variable tests. Sets RV_CAT_DB_ROLE on the
 * consumers of every role that can be rotated safely, and fills inventory->roles. */
void rv_roles_discover(RvInventory *inventory, const RvPolicy *policy);

GPtrArray *rv_plan_changes(const RvPlan *plan);          /* RvChange*, g_free each */
GPtrArray *rv_plan_deploy_targets(const RvPlan *plan);   /* RvDeployTarget* */
void       rv_deploy_target_free(RvDeployTarget *target);

void rv_progress_note(const gchar *dir, const gchar *phase, const gchar *key,
                      const gchar *status, const gchar *detail);
/* Keys with status "done" for a phase. Keys that failed and were not done later go in `failed_out`. */
GHashTable *rv_progress_done(const gchar *dir, const gchar *phase, GHashTable *failed_out);

/* The keys of RFC 5802, for the tests. */
void rv_scram_derive(const gchar *password, const guint8 *salt, gsize salt_len, guint iterations,
                     guint8 stored_key[32], guint8 server_key[32]);

gboolean rv_check_cancelled(GCancellable *cancel, GError **error);
void     rv_progress(RvProgressFunc func, gpointer data, gdouble fraction,
                     const gchar *format, ...) G_GNUC_PRINTF(4, 5);
/* Free a GString that holds secrets, wiping it first. */
void     rv_gstring_wipe_free(GString *string);
gchar   *rv_build_path(const gchar *dir, const gchar *file);
gboolean rv_read_file(const gchar *path, gchar **contents, GError **error);
/* Append one line to a file that holds no secrets, and flush it to disk. */
gboolean rv_append_line(const gchar *path, const gchar *line, GError **error);

G_END_DECLS

#endif /* RV_PRIVATE_H */
