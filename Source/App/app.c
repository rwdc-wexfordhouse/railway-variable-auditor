/*
 * Railway Variable Auditor: the GTK 4 application.
 *
 * Signs in through the Railway CLI, finds every workspace, reads every variable
 * of every service and environment, and writes them, with their values, to one
 * CSV file (audit.csv). It changes nothing in Railway.
 *
 * Screens: start, working, results.
 *
 * The program never shows a variable value. All Railway calls run in worker
 * threads, so the window never freezes.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#endif

#include "rvcore.h"

#ifdef RVC_AUTOMATION
#include "demo.h"
#endif

#define APP_ID "za.co.wexfordhouse.RailwayVariableAuditor"


#ifdef RVC_AUTOMATION
static void demo_clear_folder(const gchar *dir);
#endif

/* ---- Application state ----------------------------------------------------- */

typedef struct {
    GtkApplication *application;
    GtkWindow      *window;
    GtkStack       *stack;

    /* start */
    GtkLabel  *status_label, *folder_label;
    GtkWidget *sign_in_button, *audit_button, *choose_button;

    /* working */
    GtkLabel       *working_title, *working_message, *working_note;
    GtkProgressBar *working_bar;
    GtkWidget      *cancel_button;

    /* results */
    GtkLabel  *results_text;
    GtkWidget *folder_button;

    /* data */
    RwClient     *client;
    RvPolicy     *policy;
    gchar        *out_base;
    gchar        *run_dir;
    GCancellable *cancel;
    gboolean      busy;
    gboolean      work_running;     /* a worker thread is running */
    gchar        *policy_path;

#ifdef RVC_AUTOMATION
    /* Remote control, for taking screenshots of the documentation. Demo data only. */
    gboolean      demo;
    gchar        *screenshots_dir;
    gchar        *demo_dir;
    gpointer      demo_data;
    guint         auto_step;
    guint         auto_phase;
    gint64        auto_mark;
    gint64        auto_started;
    gboolean      auto_failed;
#endif
} App;

/* ---- Small helpers ----------------------------------------------------------- */

/* ---- Small helpers ----------------------------------------------------------- */

static void
show_page(App *app, const gchar *name)
{
    gtk_stack_set_visible_child_name(app->stack, name);
}

static void
message_dialog(App *app, const gchar *title, const gchar *detail)
{
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);

    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_show(dialog, app->window);
    g_object_unref(dialog);
}

static GtkWidget *
page_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    return box;
}

static GtkWidget *
heading(const gchar *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", text);

    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    return label;
}

static GtkWidget *
wrapped_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 90);   /* a readable line, and a bounded window */
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_selectable(GTK_LABEL(label), FALSE);
    return label;
}

static GtkWidget *
button_with_handler(const gchar *label, GCallback handler, App *app)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    g_signal_connect(button, "clicked", handler, app);
    return button;
}

static void
set_busy(App *app, gboolean busy)
{
    app->busy = busy;
    gtk_widget_set_sensitive(app->sign_in_button, !busy);
    gtk_widget_set_sensitive(app->audit_button, !busy);
    gtk_widget_set_sensitive(app->choose_button, !busy);
}

/* ---- Workers --------------------------------------------------------------------- */

typedef struct {
    App    *app;
    gchar  *message;
    gdouble fraction;
} ProgressUpdate;

static gboolean
apply_progress(gpointer data)
{
    ProgressUpdate *u = data;

    gtk_label_set_text(u->app->working_message, u->message);
    if (u->fraction >= 0)
        gtk_progress_bar_set_fraction(u->app->working_bar, u->fraction);
    else
        gtk_progress_bar_pulse(u->app->working_bar);
    g_free(u->message);
    g_free(u);
    return G_SOURCE_REMOVE;
}

static void
progress_cb(gpointer data, const gchar *message, gdouble fraction)
{
    ProgressUpdate *u = g_new0(ProgressUpdate, 1);

    u->app = data;
    u->message = g_strdup(message);
    u->fraction = fraction;
    g_idle_add(apply_progress, u);
}

static void
begin_work(App *app)
{
    gtk_label_set_text(app->working_message, "Starting");
    gtk_label_set_text(app->working_note,
        "This only reads. Nothing is changed in Railway. Railway limits requests per hour, "
        "so a large account takes a while. Cancel stops at the next safe point.");
    gtk_progress_bar_set_fraction(app->working_bar, 0);
    g_clear_object(&app->cancel);
    app->cancel = g_cancellable_new();
    gtk_widget_set_sensitive(app->cancel_button, TRUE);
    app->work_running = TRUE;
    set_busy(app, TRUE);
    show_page(app, "working");
}

static void
on_cancel(GtkButton *button, gpointer data)
{
    App *app = data;

    (void)button;
    if (app->cancel != NULL)
        g_cancellable_cancel(app->cancel);
    gtk_label_set_text(app->working_message, "Cancelling");
    gtk_widget_set_sensitive(app->cancel_button, FALSE);
}

static RwClient *
ensure_client(App *app)
{
    if (app->client == NULL)
        app->client = rw_client_new(NULL);
    return app->client;
}

/* ---- Start: sign in ------------------------------------------------------------- */

static void
status_done(GObject *source, GAsyncResult *result, gpointer data)
{
    App *app = data;
    gchar *text = g_task_propagate_pointer(G_TASK(result), NULL);

    gtk_label_set_text(app->status_label, text != NULL ? text : "");
    g_free(text);
    set_busy(app, FALSE);
    (void)source;
}

static void
status_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
    GError *error = NULL;
    gchar *version = rw_cli_version(&error), *name = NULL, *text;

    (void)source;
    (void)task_data;
    (void)cancel;
    if (version == NULL) {
        text = g_strdup(error->message);
        g_error_free(error);
    } else if (!rw_cli_whoami(&name, NULL, &error)) {
        text = g_strdup_printf("%s. Not signed in. Press \"Sign in with Railway\".", version);
        g_error_free(error);
    } else {
        text = g_strdup_printf("%s. Signed in as %s.", version, name);
    }
    g_free(version);
    g_free(name);
    g_task_return_pointer(task, text, g_free);
}

static void
refresh_status(App *app)
{
    GTask *task = g_task_new(NULL, NULL, status_done, app);

    gtk_label_set_text(app->status_label, "Checking the Railway CLI");
    set_busy(app, TRUE);
    g_task_run_in_thread(task, status_thread);
    g_object_unref(task);
}

static void
login_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
    GError *error = NULL;

    (void)source;
    (void)task_data;
    (void)cancel;
    if (rw_cli_login(&error))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, error);
}

static void
login_done(GObject *source, GAsyncResult *result, gpointer data)
{
    App *app = data;
    GError *error = NULL;

    (void)source;
    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        message_dialog(app, "Sign in did not complete", error->message);
        g_error_free(error);
    }
    refresh_status(app);
}

static void
on_sign_in(GtkButton *button, gpointer data)
{
    App *app = data;
    GTask *task = g_task_new(NULL, NULL, login_done, app);

    (void)button;
    gtk_label_set_text(app->status_label,
                       "Your browser will open so that you can sign in to Railway.");
    set_busy(app, TRUE);
    g_task_run_in_thread(task, login_thread);
    g_object_unref(task);
}
/* ---- Audit ------------------------------------------------------------------------- */

typedef struct {
    gchar *run_dir;
    gchar *summary;
} AuditResult;

static void
audit_result_free(AuditResult *r)
{
    g_free(r->run_dir);
    g_free(r->summary);
    g_free(r);
}

/* Words for the results screen. Never a value. */
static gchar *
summarise(const RvInventory *inventory, const gchar *run_dir)
{
    GString *text = g_string_new(NULL);
    guint problems = 0, shown = 0;

    g_string_append_printf(text, "Read %u variables in %u workspaces, %u projects, %u environments "
                           "and %u services.\n\n", inventory->variables->len, inventory->workspaces,
                           inventory->projects, inventory->environments, inventory->services);
    for (guint i = 0; i < inventory->report->len; i++) {
        const RvReportRow *r = inventory->report->pdata[i];

        if (g_strcmp0(r->status, "done") == 0)
            continue;
        if (problems++ == 0)
            g_string_append(text, "NOT everything was read. These were skipped or failed:\n");
        if (shown < 30) {
            g_string_append_printf(text, "  %s%s%s: %s (%s)\n", r->workspace,
                                   r->project != NULL && *r->project ? " / " : "",
                                   r->project != NULL ? r->project : "", r->status, r->detail);
            shown++;
        }
    }
    if (problems > shown)
        g_string_append_printf(text, "  and %u more.\n", problems - shown);
    if (problems > 0)
        g_string_append(text, "\n");
    g_string_append_printf(text, "Saved to:\n%s\n\n", run_dir);
    g_string_append(text,
        "audit.csv holds every variable with its current value. These are live secrets. Only "
        "you can read the file. Keep it safe, and delete it when you have finished.");
    return g_string_free(text, FALSE);
}

static void
audit_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
    App *app = task_data;
    RvScanOptions options = { ensure_client(app), app->policy, progress_cb, app, app->cancel };
    GError *error = NULL;
    RvTopology *topology;
    RvInventory *inventory;
    AuditResult *result;
    gchar *run_dir;

    (void)source;
    (void)cancel;
    topology = rv_discover(&options, &error);
    if (topology == NULL) {
        g_task_return_error(task, error);
        return;
    }
    {
        gchar *note = g_strdup_printf("About %u requests to read the variables",
                                      rv_topology_estimate(topology));

        progress_cb(app, note, -1);
        g_free(note);
    }
    inventory = rv_read_variables(topology, &options, &error);
    rv_topology_free(topology);
    if (inventory == NULL) {
        g_task_return_error(task, error);
        return;
    }
    rv_classify(inventory, app->policy);
    progress_cb(app, "Writing audit.csv", -1);
    run_dir = rv_audit_save(inventory, app->out_base, &error);
    if (run_dir == NULL) {
        rv_inventory_free(inventory);
        g_task_return_error(task, error);
        return;
    }
    result = g_new0(AuditResult, 1);
    result->summary = summarise(inventory, run_dir);
    result->run_dir = run_dir;
    rv_inventory_free(inventory);
    g_task_return_pointer(task, result, (GDestroyNotify)audit_result_free);
}

static void
audit_done(GObject *source, GAsyncResult *result, gpointer data)
{
    App *app = data;
    GError *error = NULL;
    AuditResult *audit = g_task_propagate_pointer(G_TASK(result), &error);

    (void)source;
    app->work_running = FALSE;
    set_busy(app, FALSE);
    if (audit == NULL) {
        if (!g_error_matches(error, RV_ERROR, RV_ERROR_CANCELLED))
            message_dialog(app, "The audit did not finish", error->message);
        g_error_free(error);
        show_page(app, "start");
        refresh_status(app);
        return;
    }
    g_free(app->run_dir);
    app->run_dir = g_strdup(audit->run_dir);
    gtk_label_set_text(app->results_text, audit->summary);
    audit_result_free(audit);
    show_page(app, "results");
}

static void
on_audit(GtkButton *button, gpointer data)
{
    App *app = data;
    GTask *task = g_task_new(NULL, NULL, audit_done, app);

    (void)button;
    g_task_set_task_data(task, app, NULL);
    begin_work(app);
    g_task_run_in_thread(task, audit_thread);
    g_object_unref(task);
}

/* ---- Folder and results ------------------------------------------------------------- */

static void
folder_chosen(GObject *source, GAsyncResult *result, gpointer data)
{
    App *app = data;
    GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);

    if (file == NULL)
        return;
    g_free(app->out_base);
    app->out_base = g_file_get_path(file);
    gtk_label_set_text(app->folder_label, app->out_base);
    g_object_unref(file);
}

static void
on_choose_folder(GtkButton *button, gpointer data)
{
    App *app = data;
    GtkFileDialog *dialog = gtk_file_dialog_new();

    (void)button;
    gtk_file_dialog_set_title(dialog, "Where should the audit be saved?");
    gtk_file_dialog_select_folder(dialog, app->window, NULL, folder_chosen, app);
    g_object_unref(dialog);
}

static void
open_folder_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)data;
    gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, NULL);
    g_object_unref(source);
}

static void
on_open_folder(GtkButton *button, gpointer data)
{
    App *app = data;
    GFile *file;
    GtkFileLauncher *launcher;

    (void)button;
    if (app->run_dir == NULL)
        return;
    file = g_file_new_for_path(app->run_dir);
    launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, app->window, NULL, open_folder_done, NULL);
    g_object_unref(file);
}

static void
on_audit_again(GtkButton *button, gpointer data)
{
    (void)button;
    show_page(data, "start");
    refresh_status(data);
}

/* ---- Building the window --------------------------------------------------------------- */

static GtkWidget *
row_of_buttons(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_widget_set_margin_top(row, 6);
    return row;
}

static void
build_start(App *app)
{
    GtkWidget *box = page_box(), *row = row_of_buttons(), *folder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_box_append(GTK_BOX(box), heading("Railway Variable Auditor"));
    gtk_box_append(GTK_BOX(box), wrapped_label(
        "Lists every environment variable, with its current value, in every Railway workspace "
        "you can access, in one CSV file. Use it to work through your keys by hand after the "
        "Railway incident. It only reads. Nothing is changed in Railway."));
    app->status_label = GTK_LABEL(wrapped_label(""));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->status_label));

    gtk_box_append(GTK_BOX(box), wrapped_label(
        "What happens:\n"
        "1.  Sign in. Your browser opens, and the program never sees your token.\n"
        "2.  Audit. Every workspace, project, environment and service is read.\n"
        "3.  You get one file, audit.csv, with every variable and its value. Anything "
        "that could not be read is listed on the screen.\n\n"
        "audit.csv holds live secrets. Only you can read it. Delete it when you have finished."));

    gtk_box_append(GTK_BOX(folder_row), gtk_label_new("Save in:"));
    app->folder_label = GTK_LABEL(gtk_label_new(app->out_base));
    gtk_label_set_ellipsize(app->folder_label, PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(GTK_WIDGET(app->folder_label), TRUE);
    gtk_label_set_xalign(app->folder_label, 0);
    gtk_box_append(GTK_BOX(folder_row), GTK_WIDGET(app->folder_label));
    app->choose_button = button_with_handler("Choose…", G_CALLBACK(on_choose_folder), app);
    gtk_box_append(GTK_BOX(folder_row), app->choose_button);
    gtk_box_append(GTK_BOX(box), folder_row);

    app->sign_in_button = button_with_handler("Sign in with Railway", G_CALLBACK(on_sign_in), app);
    app->audit_button = button_with_handler("Audit all my workspaces", G_CALLBACK(on_audit), app);
    gtk_widget_add_css_class(app->audit_button, "suggested-action");
    gtk_box_append(GTK_BOX(row), app->sign_in_button);
    gtk_box_append(GTK_BOX(row), app->audit_button);
    gtk_box_append(GTK_BOX(box), row);
    gtk_stack_add_named(app->stack, box, "start");
}

static void
build_working(App *app)
{
    GtkWidget *box = page_box();

    app->working_title = GTK_LABEL(heading("Reading your workspaces"));
    app->working_message = GTK_LABEL(wrapped_label(""));
    app->working_note = GTK_LABEL(wrapped_label(""));
    gtk_widget_add_css_class(GTK_WIDGET(app->working_note), "dim-label");
    app->working_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    app->cancel_button = button_with_handler("Cancel", G_CALLBACK(on_cancel), app);
    gtk_widget_set_halign(app->cancel_button, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->working_title));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->working_bar));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->working_message));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->working_note));
    gtk_box_append(GTK_BOX(box), app->cancel_button);
    gtk_stack_add_named(app->stack, box, "working");
}

static void
build_results(App *app)
{
    GtkWidget *box = page_box(), *row = row_of_buttons();

    gtk_box_append(GTK_BOX(box), heading("Audit finished"));
    app->results_text = GTK_LABEL(wrapped_label(""));
    gtk_label_set_selectable(app->results_text, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(app->results_text));

    app->folder_button = button_with_handler("Open the folder", G_CALLBACK(on_open_folder), app);
    gtk_widget_add_css_class(app->folder_button, "suggested-action");
    gtk_box_append(GTK_BOX(row), app->folder_button);
    gtk_box_append(GTK_BOX(row), button_with_handler("Audit again", G_CALLBACK(on_audit_again), app));
    gtk_box_append(GTK_BOX(box), row);
    gtk_stack_add_named(app->stack, box, "results");
}

static void
app_free(App *app)
{
#ifdef RVC_AUTOMATION
    g_free(app->screenshots_dir);
    if (app->demo_dir != NULL)
        demo_clear_folder(app->demo_dir);
    g_free(app->demo_dir);
    demo_free(app->demo_data);
#endif
    g_free(app->policy_path);
    g_clear_pointer(&app->policy, rv_policy_free);
    g_clear_pointer(&app->client, rw_client_free);
    g_clear_object(&app->cancel);
    g_free(app->out_base);
    g_free(app->run_dir);
    g_free(app);
}

#ifdef RVC_AUTOMATION
/* ---- Remote control: drive the real window and save a PNG of each screen ---------
 * Built only with -Dautomation=true. Runs on demonstration data, never on a real
 * account. Used to make the screenshots in the documentation. */

typedef struct {
    const gchar *name;
    void       (*act)(App *app);
    gboolean   (*ready)(App *app);
    guint        settle_ms;
    const gchar *shot;
    GtkWindow *(*window)(App *app);     /* the window to photograph, or NULL for the main one */
} AutoStep;

/* The demonstration folder has a fixed, tidy name, so it is cleared before each run. */
static void
demo_clear_folder(const gchar *dir)
{
    GDir *top = g_dir_open(dir, 0, NULL);
    const gchar *name;

    while (top != NULL && (name = g_dir_read_name(top)) != NULL) {
        gchar *path = g_build_filename(dir, name, NULL);
        GDir *inner = g_dir_open(path, 0, NULL);
        const gchar *file;

        while (inner != NULL && (file = g_dir_read_name(inner)) != NULL) {
            gchar *child = g_build_filename(path, file, NULL);

            g_remove(child);
            g_free(child);
        }
        if (inner != NULL)
            g_dir_close(inner);
        g_remove(path);
        g_free(path);
    }
    if (top != NULL)
        g_dir_close(top);
}

static gboolean
on_page(App *app, const gchar *page)
{
    return g_strcmp0(gtk_stack_get_visible_child_name(app->stack), page) == 0;
}

static gboolean ready_start(App *a)   { return on_page(a, "start") && !a->busy; }
static gboolean ready_working(App *a) { return on_page(a, "working"); }
static gboolean ready_results(App *a) { return on_page(a, "results") && !a->work_running; }

static void act_audit(App *a)            { demo_set_failing_project(a->demo_data, NULL); on_audit(NULL, a); }
static void act_audit_incomplete(App *a) { demo_set_failing_project(a->demo_data, "p4"); on_audit(NULL, a); }
static void act_again(App *a)            { on_audit_again(NULL, a); }

static const AutoStep auto_steps[] = {
    { "start",            NULL,                 ready_start,   500,  "01-start" },
    { "audit",            act_audit,            ready_working, 1000, "02-working" },
    { "results",          NULL,                 ready_results, 500,  "03-results" },
    { "again",            act_again,            ready_start,   200,  NULL },
    { "audit incomplete", act_audit_incomplete, ready_results, 500,  "04-results-incomplete", NULL },
};

static gboolean
capture(App *app, GtkWindow *window, const gchar *name)
{
    GtkWidget *widget = GTK_WIDGET(window);
    int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
    GdkPaintable *paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    GskRenderNode *node, *scaled;
    GskTransform *transform;
    const float scale = 2.0f;          /* sharp text, even when the picture is shown smaller */
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width * scale, height * scale);
    GdkTexture *texture;
    gchar *file = g_strdup_printf("%s.png", name);
    gchar *path = g_build_filename(app->screenshots_dir, file, NULL);
    gboolean ok = FALSE;

    gdk_paintable_snapshot(paintable, (GdkSnapshot *)snapshot, width, height);
    node = gtk_snapshot_free_to_node(snapshot);
    if (node != NULL) {
        transform = gsk_transform_scale(NULL, scale, scale);
        scaled = gsk_transform_node_new(node, transform);
        gsk_transform_unref(transform);
        texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)),
                                              scaled, &bounds);
        if (texture != NULL) {
            ok = gdk_texture_save_to_png(texture, path);
            g_object_unref(texture);
        }
        gsk_render_node_unref(scaled);
        gsk_render_node_unref(node);
    }
    g_print("%s %s (%dx%d at 2x)\n", ok ? "saved" : "FAILED", path, width, height);
    g_object_unref(paintable);
    g_free(path);
    g_free(file);
    return ok;
}

static gboolean
auto_tick(gpointer data)
{
    App *app = data;
    gint64 now = g_get_monotonic_time();
    const AutoStep *step;

    if (app->auto_step >= G_N_ELEMENTS(auto_steps)) {
        g_application_quit(G_APPLICATION(app->application));
        return G_SOURCE_REMOVE;
    }
    step = &auto_steps[app->auto_step];

    if (app->auto_phase == 0) {
        app->auto_started = now;
        if (step->act != NULL)
            step->act(app);
        app->auto_phase = 1;
    } else if (app->auto_phase == 1) {
        if (step->ready(app)) {
            app->auto_phase = 2;
            app->auto_mark = now;
        } else if (now - app->auto_started > 60 * G_USEC_PER_SEC) {
            g_printerr("Step \"%s\" did not finish in 60 seconds.\n", step->name);
            app->auto_failed = TRUE;
            g_application_quit(G_APPLICATION(app->application));
            return G_SOURCE_REMOVE;
        }
    } else if (now - app->auto_mark >= (gint64)step->settle_ms * 1000) {
        if (step->shot != NULL && !capture(app, step->window != NULL ? step->window(app) : app->window, step->shot))
            app->auto_failed = TRUE;
        app->auto_step++;
        app->auto_phase = 0;
    }
    return G_SOURCE_CONTINUE;
}

static void
automation_start(App *app)
{
    if (app->screenshots_dir == NULL)
        return;
    g_mkdir_with_parents(app->screenshots_dir, 0755);
    g_timeout_add(200, auto_tick, app);
}

static gint
on_local_options(GApplication *application, GVariantDict *options, gpointer data)
{
    App *app = data;
    const gchar *dir = NULL;

    (void)application;
    if (g_variant_dict_contains(options, "demo"))
        app->demo = TRUE;
    if (g_variant_dict_lookup(options, "screenshots", "^&ay", &dir)) {
        app->screenshots_dir = g_strdup(dir);
        app->demo = TRUE;
    }
    return -1;
}
#endif /* RVC_AUTOMATION */

/* Closing the window while a worker thread uses the data would free it underneath it. */
static gboolean
on_close_request(GtkWindow *window, gpointer data)
{
    App *app = data;

    (void)window;
    if (!app->work_running)
        return FALSE;
    message_dialog(app, "The audit is still running",
                   "Press Cancel on the working screen and wait for it to stop, or wait for it "
                   "to finish.");
    return TRUE;
}

static void
on_activate(GtkApplication *application, gpointer data)
{
    App *app = data;
    GtkWidget *header;
    GError *error = NULL;
    gchar *policy_path;

    if (app->window != NULL) {
        gtk_window_present(app->window);
        return;
    }
    app->application = application;
    app->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(app->window, "Railway Variable Auditor");
    gtk_window_set_default_size(app->window, 820, 700);
    g_signal_connect(app->window, "close-request", G_CALLBACK(on_close_request), app);

#ifdef RVC_AUTOMATION
    if (app->demo) {
        /* Demo data only: its own folder and policy, never the user's. */
        app->demo_dir = g_build_filename(g_get_tmp_dir(), "railway-variable-auditor-demo", NULL);
        demo_clear_folder(app->demo_dir);
        g_mkdir_with_parents(app->demo_dir, 0700);
        app->policy_path = g_build_filename(app->demo_dir, "policy.ini", NULL);
        gtk_window_set_default_size(app->window, 800, 800);   /* square, the same for every picture */
        gtk_window_set_title(app->window, "Railway Variable Auditor (demonstration data)");
    } else
#endif
    app->policy_path = rv_policy_default_path();
    policy_path = g_strdup(app->policy_path);
    app->policy = rv_policy_load(policy_path, &error);
    if (app->policy == NULL) {
        g_warning("%s", error->message);
        g_clear_error(&error);
        app->policy = rv_policy_new();
    }
    g_free(policy_path);
    app->out_base = rv_audit_default_folder();
#ifdef RVC_AUTOMATION
    if (app->demo) {
        g_free(app->out_base);
        app->out_base = g_strdup(app->demo_dir);
        app->demo_data = demo_new();
        app->client = rw_client_new("demo");
        rw_client_set_transport(app->client, demo_send, app->demo_data, NULL);
    }
#endif

    header = gtk_header_bar_new();
    gtk_window_set_titlebar(app->window, header);

    app->stack = GTK_STACK(gtk_stack_new());
    build_start(app);
    build_working(app);
    build_results(app);
    gtk_window_set_child(app->window, GTK_WIDGET(app->stack));
    show_page(app, "start");
    gtk_window_present(app->window);
#ifdef RVC_AUTOMATION
    if (app->demo) {
        gtk_label_set_text(app->status_label,
                           "Railway CLI (demonstration). Signed in as Demo User. "
                           "Everything on this screen is invented.");
        automation_start(app);
        return;
    }
#endif
    refresh_status(app);
}

int
main(int argc, char **argv)
{
    App *app = g_new0(App, 1);
    GtkApplication *application = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    int status;

#ifdef G_OS_WIN32
    /*
     * A GUI program has no console, and from GLib's console-less spawn path the
     * Railway CLI exits 0 with no output. Give the window a hidden console of its
     * own so the CLI inherits it. rw_cli_login() shows it while the CLI asks for input.
     */
    if (GetConsoleWindow() == NULL && AllocConsole())
        ShowWindow(GetConsoleWindow(), SW_HIDE);
#endif

    g_signal_connect(application, "activate", G_CALLBACK(on_activate), app);
#ifdef RVC_AUTOMATION
    g_application_add_main_option(G_APPLICATION(application), "demo", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE,
                                  "Use invented demonstration data. Nothing is read from or sent to Railway", NULL);
    g_application_add_main_option(G_APPLICATION(application), "screenshots", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_FILENAME,
                                  "Drive every screen on demonstration data, save a PNG of each in DIR, and quit",
                                  "DIR");
    g_signal_connect(application, "handle-local-options", G_CALLBACK(on_local_options), app);
#endif
    status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
#ifdef RVC_AUTOMATION
    if (app->auto_failed && status == 0)
        status = 1;
#endif
    app_free(app);
    return status;
}
