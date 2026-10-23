/*
 * Live test of the real database interface (libpq, loaded at run time) against a
 * real PostgreSQL. Run by tests/postgres_live_test.sh, which starts a throwaway server.
 * Skipped (exit 77) when no server is given.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rvcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, what) do { \
    if (cond) printf("ok:   %s\n", what); \
    else { printf("FAIL: %s\n", what); failures++; } } while (0)

static gboolean
login(const RvDbOps *db, guint port, const gchar *database, const gchar *user, const gchar *pw, GError **error)
{
    return db->login_ok(NULL, "127.0.0.1", port, database, user, pw, error);
}

static gboolean
set_pw(const RvDbOps *db, guint port, const gchar *admin_pw, const gchar *role, const gchar *plain, GError **error)
{
    gchar *verifier = rv_scram_verifier(plain, error);
    gboolean ok;

    if (verifier == NULL)
        return FALSE;
    ok = db->set_password(NULL, "127.0.0.1", port, "postgres", "postgres", admin_pw, role, verifier, error);
    g_free(verifier);
    return ok;
}

int
main(void)
{
    const gchar *port_text = g_getenv("RVC_PG_PORT"), *admin = g_getenv("RVC_PG_ADMIN_PASSWORD");
    const RvDbOps *db = rv_db_ops_libpq();
    GError *error = NULL;
    guint port;

    if (port_text == NULL || admin == NULL) {
        printf("no server given, skipping\n");
        return 77;
    }
    port = (guint)atoi(port_text);

    CHECK(rv_libpq_available(&error), "libpq loads at run time");
    if (error != NULL) { printf("      %s\n", error->message); g_clear_error(&error); return 1; }

    CHECK(login(db, port, "postgres", "postgres", admin, &error), "the admin can log in");
    g_clear_error(&error);

    /* The role was made by the script with the password "OldPassword0123456789". */
    CHECK(login(db, port, "ledgerdb", "ledger", "OldPassword0123456789", &error), "the role logs in with its current password");
    g_clear_error(&error);

    CHECK(!login(db, port, "ledgerdb", "ledger", "WrongPassword0123456789", &error) && g_error_matches(error, RV_ERROR, RV_ERROR_DB_AUTH),
          "a wrong password is reported as a refused login");
    g_clear_error(&error);
    CHECK(!login(db, port, "ledgerdb", "nosuchrole", "x", &error) && g_error_matches(error, RV_ERROR, RV_ERROR_DB_AUTH),
          "an unknown role is reported as a refused login");
    g_clear_error(&error);
    CHECK(!login(db, port, "nosuchdb", "ledger", "OldPassword0123456789", &error) && g_error_matches(error, RV_ERROR, RV_ERROR_DB),
          "a wrong database is a database error, not a refused login");
    g_clear_error(&error);

    /* Change it with a SCRAM-SHA-256 hash, never the plain text. */
    CHECK(set_pw(db, port, admin, "ledger", "NewPassword9876543210abcdefgh", &error), "ALTER ROLE with a SCRAM hash is accepted");
    if (error != NULL) { printf("      %s\n", error->message); g_clear_error(&error); }
    CHECK(login(db, port, "ledgerdb", "ledger", "NewPassword9876543210abcdefgh", &error), "the new password logs in");
    g_clear_error(&error);
    CHECK(!login(db, port, "ledgerdb", "ledger", "OldPassword0123456789", &error) && g_error_matches(error, RV_ERROR, RV_ERROR_DB_AUTH),
          "the old password no longer logs in");
    g_clear_error(&error);

    /* And put it back, as an undo does. */
    CHECK(set_pw(db, port, admin, "ledger", "OldPassword0123456789", &error), "the old password can be set back");
    g_clear_error(&error);
    CHECK(login(db, port, "ledgerdb", "ledger", "OldPassword0123456789", &error), "the old password logs in again");
    g_clear_error(&error);

    /* A role name with a space and a quote must be quoted properly. */
    CHECK(set_pw(db, port, admin, "Odd Role\"x", "OddRolePassword0123456789", &error), "a role name with a space and a quote is quoted correctly");
    g_clear_error(&error);
    CHECK(login(db, port, "postgres", "Odd Role\"x", "OddRolePassword0123456789", &error), "and that role logs in with the new password");
    g_clear_error(&error);

    /* A wrong admin password must not change anything. */
    CHECK(!set_pw(db, port, "not-the-admin-password", "ledger", "ShouldNeverBeSet0123456789", &error), "a wrong admin password is refused");
    g_clear_error(&error);
    CHECK(login(db, port, "ledgerdb", "ledger", "OldPassword0123456789", &error), "and the role is unchanged");
    g_clear_error(&error);

    /* A role that does not exist. */
    CHECK(!set_pw(db, port, admin, "norolehere", "SomePassword0123456789", &error) && g_error_matches(error, RV_ERROR, RV_ERROR_DB),
          "changing the password of a role that does not exist fails cleanly");
    g_clear_error(&error);

    printf("%s\n", failures == 0 ? "all live checks passed" : "LIVE CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
