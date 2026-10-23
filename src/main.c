#include <adwaita.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

#define APP_ID "org.damavand.Welcome"
#define MARKER ".setup/firstboot.done"

typedef struct {
    AdwApplication *app;
    AdwApplicationWindow *window;

    GtkEntry *hostname;
    GtkPasswordEntry *user_password;
    GtkPasswordEntry *user_confirm;
    GtkPasswordEntry *root_password;
    GtkPasswordEntry *root_confirm;

    GtkDropDown *locale;
    GtkDropDown *timezone;
    GtkDropDown *keyboard;
    GtkDropDown *theme;

    GtkStringList *locales;
    GtkStringList *timezones;
    GtkStringList *keyboards;
} Setup;

static Setup ui;

static char *
run_capture(const char *program, const char *arg)
{
    const char *argv[] = { program, arg, NULL };
    g_autoptr(GSubprocess) proc = NULL;
    g_autofree char *out = NULL;
    g_autofree char *err = NULL;

    proc = g_subprocess_newv(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE,
        NULL
    );

    if (!proc)
        return g_strdup("");

    if (!g_subprocess_communicate_utf8(
            proc, NULL, NULL, &out, &err, NULL))
        return g_strdup("");

    if (!g_subprocess_get_successful(proc))
        return g_strdup("");

    return g_steal_pointer(&out);
}

static GtkStringList *
load_lines(const char *program, const char *arg, gboolean locale_filter)
{
    g_autofree char *output = run_capture(program, arg);
    GtkStringList *list = gtk_string_list_new(NULL);
    g_auto(GStrv) lines = g_strsplit(output, "\n", -1);

    for (guint i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);

        if (!*line)
            continue;

        if (locale_filter) {
            if (!strstr(line, "UTF-8") &&
                !strstr(line, "utf8") &&
                !strstr(line, "utf-8"))
                continue;
        }

        gtk_string_list_append(list, line);
    }

    return list;
}

static guint
find_item(GtkStringList *list, const char *value)
{
    for (guint i = 0; i < g_list_model_get_n_items(
             G_LIST_MODEL(list)); i++) {
        const char *item = gtk_string_list_get_string(list, i);

        if (g_strcmp0(item, value) == 0)
            return i;
    }

    return GTK_INVALID_LIST_POSITION;
}

static char *
selected_string(GtkDropDown *drop, GtkStringList *model)
{
    guint index = gtk_drop_down_get_selected(drop);

    if (index == GTK_INVALID_LIST_POSITION)
        return NULL;

    return g_strdup(gtk_string_list_get_string(model, index));
}

static gboolean
marker_exists(void)
{
    const char *home = g_get_home_dir();
    g_autofree char *path = g_build_filename(home, MARKER, NULL);

    return g_file_test(path, G_FILE_TEST_EXISTS);
}

/* Execute a privileged command. Arguments are passed directly,
 * without invoking a shell. */
static gboolean
run_privileged(const char *const *args, const char *input,
               char **error_out)
{
    g_autoptr(GSubprocess) proc = NULL;
    g_autofree char *out = NULL;
    g_autofree char *err = NULL;
    g_autoptr(GError) error = NULL;

    proc = g_subprocess_newv(
        args,
        G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE,
        &error
    );

    if (!proc) {
        *error_out = g_strdup(error->message);
        return FALSE;
    }

    if (!g_subprocess_communicate_utf8(
            proc, input, NULL, &out, &err, &error)) {
        *error_out = g_strdup(error->message);
        return FALSE;
    }

    if (!g_subprocess_get_successful(proc)) {
        *error_out = g_strdup(
            err && *err ? err : "Command failed."
        );
        return FALSE;
    }

    return TRUE;
}

static void
show_message(const char *title, const char *message)
{
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(
        adw_alert_dialog_new(title, message)
    );

    adw_alert_dialog_add_response(dialog, "ok", "OK");
    adw_alert_dialog_set_default_response(dialog, "ok");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(ui.window));
}

static gboolean
valid_hostname(const char *name)
{
    gsize len = strlen(name);

    if (len < 1 || len > 253)
        return FALSE;

    if (name[0] == '-' || name[len - 1] == '-')
        return FALSE;

    for (gsize i = 0; i < len; i++) {
        char c = name[i];

        if (!(g_ascii_isalnum(c) || c == '-' || c == '.'))
            return FALSE;
    }

    return TRUE;
}

static gboolean
create_marker(char **error_out)
{
    g_autofree char *directory =
        g_build_filename(g_get_home_dir(), ".setup", NULL);
    g_autofree char *path =
        g_build_filename(directory, "firstboot.done", NULL);

    if (g_mkdir_with_parents(directory, 0700) != 0) {
        *error_out = g_strdup("Could not create the setup directory.");
        return FALSE;
    }

    if (!g_file_set_contents(path, "completed\n", -1, NULL)) {
        *error_out = g_strdup("Could not create the completion marker.");
        return FALSE;
    }

    chmod(path, 0600);
    return TRUE;
}

static void
apply_setup(GtkButton *button, gpointer data)
{
    g_autofree char *host =
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(ui.hostname)));
    g_autofree char *pw =
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(ui.user_password)));
    g_autofree char *pw2 =
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(ui.user_confirm)));
    g_autofree char *rpw =
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(ui.root_password)));
    g_autofree char *rpw2 =
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(ui.root_confirm)));
    g_autofree char *loc =
        selected_string(ui.locale, ui.locales);
    g_autofree char *zone =
        selected_string(ui.timezone, ui.timezones);
    g_autofree char *layout =
        selected_string(ui.keyboard, ui.keyboards);

    g_autoptr(GError) error = NULL;
    g_autofree char *details = NULL;

    if (marker_exists()) {
        show_message("Setup already completed",
                     "DamavandOS has already been configured.");
        return;
    }

    if (!valid_hostname(host)) {
        show_message("Invalid hostname",
                     "Use letters, numbers, dots and hyphens.");
        return;
    }

    if (!*pw || g_strcmp0(pw, pw2) != 0) {
        show_message("Password error",
                     "The user password is empty or the passwords do not match.");
        return;
    }

    if (!*rpw || g_strcmp0(rpw, rpw2) != 0) {
        show_message("Password error",
                     "The root password is empty or the passwords do not match.");
        return;
    }

    if (!loc || !zone || !layout) {
        show_message("Missing selection",
                     "Select a language, time zone and keyboard layout.");
        return;
    }

    /*
     * Apply privileged system configuration.
     * pkexec provides the graphical authentication prompt.
     */
    const char *host_cmd[] = {
        "pkexec", "hostnamectl", "set-hostname", host, NULL
    };

    if (!run_privileged(host_cmd, NULL, &details))
        goto failed;

    g_clear_pointer(&details, g_free);

    const char *zone_cmd[] = {
        "pkexec", "timedatectl", "set-timezone", zone, NULL
    };

    if (!run_privileged(zone_cmd, NULL, &details))
        goto failed;

    g_clear_pointer(&details, g_free);

    g_autofree char *locale_arg = g_strdup_printf("LANG=%s", loc);
    const char *locale_cmd[] = {
        "pkexec", "localectl", "set-locale", locale_arg, NULL
    };

    if (!run_privileged(locale_cmd, NULL, &details))
        goto failed;

    g_clear_pointer(&details, g_free);

    /*
     * Passwords are sent over stdin, not placed in command arguments.
     */
    struct passwd *account = getpwuid(getuid());

    if (!account || !account->pw_name) {
        details = g_strdup("Could not determine the current username.");
        goto failed;
    }

    g_autofree char *user_line =
        g_strdup_printf("%s:%s\n", account->pw_name, pw);
    const char *user_pw_cmd[] = { "pkexec", "chpasswd", NULL };

    if (!run_privileged(user_pw_cmd, user_line, &details))
        goto failed;

    g_clear_pointer(&details, g_free);

    g_autofree char *root_line =
        g_strdup_printf("root:%s\n", rpw);
    const char *root_pw_cmd[] = { "pkexec", "chpasswd", NULL };

    if (!run_privileged(root_pw_cmd, root_line, &details))
        goto failed;

    /*
     * Per-user GNOME settings.
     */
    GSettings *input_settings = g_settings_new(
        "org.gnome.desktop.input-sources"
    );

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ss)"));
    g_variant_builder_add(&builder, "(ss)", "xkb", layout);

    g_settings_set_value(
        input_settings,
        "sources",
        g_variant_builder_end(&builder)
    );

    g_settings_set_uint(input_settings, "current", 0);
    g_object_unref(input_settings);

    GSettings *appearance = g_settings_new(
        "org.gnome.desktop.interface"
    );

    guint theme_index = gtk_drop_down_get_selected(ui.theme);
    const char *scheme = theme_index == 1
        ? "prefer-dark" : "default";

    g_settings_set_string(appearance, "color-scheme", scheme);
    g_object_unref(appearance);

    /*
     * Only create the marker after all required steps succeed.
     */
    if (!create_marker(&details))
        goto failed;

    show_message("Setup complete 🎉",
                 "Your DamavandOS configuration has been applied.");
    gtk_window_close(GTK_WINDOW(ui.window));
    return;

failed:
    show_message("Setup failed",
                 details ? details : "An unknown error occurred.");
}

static GtkWidget *
make_row(const char *title, GtkWidget *control)
{
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(row), title
    );

    gtk_widget_set_size_request(control, 230, -1);
    adw_action_row_add_suffix(row, control);
    adw_action_row_set_activatable_widget(row, control);

    return GTK_WIDGET(row);
}

static void
activate(AdwApplication *app, gpointer data)
{
    if (marker_exists()) {
        AdwAlertDialog *dialog = ADW_ALERT_DIALOG(
            adw_alert_dialog_new(
                "Setup already completed",
                "The first-boot setup has already finished."
            )
        );

        adw_alert_dialog_add_response(dialog, "ok", "OK");
        adw_dialog_present(ADW_DIALOG(dialog), NULL);
        return;
    }

    ui.window = ADW_APPLICATION_WINDOW(
        adw_application_window_new(GTK_APPLICATION(app))
    );

    gtk_window_set_title(GTK_WINDOW(ui.window), "DamavandOS Welcome");
    gtk_window_set_default_size(GTK_WINDOW(ui.window), 760, 720);

    AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
    adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

    gtk_widget_set_margin_top(page, 24);
    gtk_widget_set_margin_bottom(page, 24);
    gtk_widget_set_margin_start(page, 28);
    gtk_widget_set_margin_end(page, 28);

    GtkWidget *title = gtk_label_new("Welcome to DamavandOS 👋");
    gtk_widget_add_css_class(title, "title-1");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(page), title);

    GtkWidget *subtitle = gtk_label_new(
        "Finish setting up your system. Choose from the options available on this computer."
    );
    gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(page), subtitle);

    AdwPreferencesGroup *system_group =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(system_group, "System");

    ui.hostname = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(ui.hostname), "damavandos");
    gtk_entry_set_placeholder_text(ui.hostname, "Computer name");
    adw_preferences_group_add(
        system_group, make_row("Hostname", GTK_WIDGET(ui.hostname))
    );

    ui.locales = load_lines("locale", "-a", TRUE);
    ui.locale = GTK_DROP_DOWN(gtk_drop_down_new(
        G_LIST_MODEL(g_object_ref(ui.locales)), NULL
    ));

    adw_preferences_group_add(
        system_group, make_row("System language", GTK_WIDGET(ui.locale))
    );

    ui.timezones = load_lines("timedatectl", "list-timezones", FALSE);
    ui.timezone = GTK_DROP_DOWN(gtk_drop_down_new(
        G_LIST_MODEL(g_object_ref(ui.timezones)), NULL
    ));

    adw_preferences_group_add(
        system_group, make_row("Time zone", GTK_WIDGET(ui.timezone))
    );

    ui.keyboards = load_lines(
        "localectl", "list-x11-keymap-layouts", FALSE
    );
    ui.keyboard = GTK_DROP_DOWN(gtk_drop_down_new(
        G_LIST_MODEL(g_object_ref(ui.keyboards)), NULL
    ));

    adw_preferences_group_add(
        system_group, make_row("Keyboard layout", GTK_WIDGET(ui.keyboard))
    );

    gtk_box_append(GTK_BOX(page), GTK_WIDGET(system_group));

    AdwPreferencesGroup *account_group =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(account_group, "Passwords");

    ui.user_password = GTK_PASSWORD_ENTRY(gtk_password_entry_new());
    gtk_password_entry_set_show_peek_icon(ui.user_password, TRUE);
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(ui.user_password), "New user password"
    );

    ui.user_confirm = GTK_PASSWORD_ENTRY(gtk_password_entry_new());
    gtk_password_entry_set_show_peek_icon(ui.user_confirm, TRUE);
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(ui.user_confirm), "Confirm user password"
    );

    ui.root_password = GTK_PASSWORD_ENTRY(gtk_password_entry_new());
    gtk_password_entry_set_show_peek_icon(ui.root_password, TRUE);
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(ui.root_password), "New root password"
    );

    ui.root_confirm = GTK_PASSWORD_ENTRY(gtk_password_entry_new());
    gtk_password_entry_set_show_peek_icon(ui.root_confirm, TRUE);
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(ui.root_confirm), "Confirm root password"
    );

    adw_preferences_group_add(account_group,
        make_row("User password", GTK_WIDGET(ui.user_password)));
    adw_preferences_group_add(account_group,
        make_row("Confirm user password", GTK_WIDGET(ui.user_confirm)));
    adw_preferences_group_add(account_group,
        make_row("Root password", GTK_WIDGET(ui.root_password)));
    adw_preferences_group_add(account_group,
        make_row("Confirm root password", GTK_WIDGET(ui.root_confirm)));

    gtk_box_append(GTK_BOX(page), GTK_WIDGET(account_group));

    AdwPreferencesGroup *appearance_group =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(appearance_group, "Appearance");

    const char *themes[] = { "Light", "Dark", NULL };
    ui.theme = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(themes));

    adw_preferences_group_add(appearance_group,
        make_row("Color scheme", GTK_WIDGET(ui.theme)));
    gtk_box_append(GTK_BOX(page), GTK_WIDGET(appearance_group));

    GtkWidget *finish = gtk_button_new_with_label("Apply and Finish");
    gtk_widget_add_css_class(finish, "suggested-action");
    gtk_widget_add_css_class(finish, "pill");
    gtk_widget_set_halign(finish, GTK_ALIGN_END);
    g_signal_connect(finish, "clicked", G_CALLBACK(apply_setup), NULL);
    gtk_box_append(GTK_BOX(page), finish);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page);
    adw_toolbar_view_set_content(toolbar, scroll);
    adw_application_window_set_content(
        ui.window, GTK_WIDGET(toolbar)
    );

    gtk_window_present(GTK_WINDOW(ui.window));
}

int
main(int argc, char **argv)
{
    ui.app = ADW_APPLICATION(adw_application_new(
        APP_ID, G_APPLICATION_DEFAULT_FLAGS
    ));

    g_signal_connect(ui.app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(
        G_APPLICATION(ui.app), argc, argv
    );

    g_object_unref(ui.app);
    return status;
}
