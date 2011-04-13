/* Copyright 2011 by Yasuhiro Matsumoto
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <gtk/gtk.h>
#ifdef _WIN32
# include <gdk/gdkwin32.h>
# include <ws2tcpip.h>
#else
# include <netinet/tcp.h>
# include <sys/socket.h>
# include <netdb.h>
# include <unistd.h>
#endif
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <sqlite3.h>
#ifdef _WIN32
# include <io.h>
#endif
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/des.h>
#include "gol.h"

#ifdef _WIN32
typedef char sockopt_t;
typedef int socklen_t;
# ifndef snprintf
#  define snprintf _snprintf
# endif
# ifndef strncasecmp
#  define strncasecmp(d,s,n) strnicmp(d,s,n)
# endif
# ifndef srandom
#  define srandom(s) srand(s)
# endif
# ifndef random
#  define random() rand()
# endif
#else
# define closesocket(x) close(x)
typedef int sockopt_t;
#endif


typedef struct {
  gboolean (*init)();
  gboolean (*show)(NOTIFICATION_INFO* ni);
  gboolean (*term)();
  gchar* (*name)();
  gchar* (*description)();
} PLUGIN_INFO;

static gchar* password = NULL;
static sqlite3 *db = NULL;
static GtkStatusIcon* status_icon = NULL;
static GList* plugin_list = NULL;
static PLUGIN_INFO* current_plugin = NULL;
static gchar* exepath = NULL;

#ifndef DATADIR
# define DATADIR exepath
#endif

static long
readall(int fd, char** ptr) {
  int i = 0, r;
  *ptr = (char*) calloc(BUFSIZ, 1);
  while (*ptr && (r = recv(fd, *ptr + i, BUFSIZ, 0)) > 0) {
    i += r;
    if (r > 2 && !strncmp(*ptr + i - 4, "\r\n\r\n", 4)) break;
    *ptr = realloc(*ptr, BUFSIZ + i);
  }
  return i;
}

unsigned int
unhex(unsigned char c) {
  if('0' <= c && c <= '9') return (c - '0');
  if('a' <= c && c <= 'f') return (0x0a + c - 'a');
  if('A' <= c && c <= 'F') return (0x0a + c - 'A');
  return 0;
}

static void
status_icon_popup(GtkStatusIcon* status_icon, guint button, guint32 activate_time, gpointer menu) {
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, status_icon, button, activate_time);
}

static void
tree_selection_changed(GtkTreeSelection *selection, gpointer data) {
  GtkTreeIter iter;
  GtkTreeModel* model;
  if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
    gchar* name;
    gtk_tree_model_get(model, &iter, 0, &name, -1);
    int i, len = g_list_length(plugin_list);
    for (i = 0; i < len; i++) {
      PLUGIN_INFO* pi = (PLUGIN_INFO*) g_list_nth_data(plugin_list, i);
      if (!g_strcasecmp(pi->name(), name)) {
        current_plugin = pi;
        break;
      }
    }
    GtkWidget* label = (GtkWidget*) g_object_get_data(G_OBJECT(data), "description");
    gtk_label_set_markup(GTK_LABEL(label), "");
    gtk_label_set_markup(GTK_LABEL(label), current_plugin->description());
    g_free(name);
  }
}

static void
settings_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  GtkWidget* dialog;
  GtkWidget* notebook;

  dialog = gtk_dialog_new_with_buttons(
      "Settings", NULL, GTK_DIALOG_MODAL,
      GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
  gchar* path = g_build_filename(DATADIR, "data", "icon.png", NULL);
  gtk_window_set_icon_from_file(GTK_WINDOW(dialog), path, NULL);
  g_free(path);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);

  notebook = gtk_notebook_new();
  gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), notebook);
  
  {
    GtkWidget* hbox = gtk_hbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), hbox, gtk_label_new("Display"));
    GtkListStore* model = (GtkListStore *)gtk_list_store_new(1, G_TYPE_STRING, GDK_TYPE_DISPLAY);
    GtkWidget* tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model));
    GtkTreeSelection* select = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
    g_signal_connect(G_OBJECT(select), "changed", G_CALLBACK(tree_selection_changed), dialog);
    gtk_box_pack_start(GTK_BOX(hbox), tree_view, FALSE, FALSE, 0);
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(
        "Name", gtk_cell_renderer_text_new(), "text", 0, NULL);
    gtk_tree_view_append_column (GTK_TREE_VIEW (tree_view), column);

    GtkTreeIter iter;
    int i, len = g_list_length(plugin_list);
    for (i = 0; i < len; i++) {
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      PLUGIN_INFO* pi = (PLUGIN_INFO*) g_list_nth_data(plugin_list, i);
      gtk_list_store_set(GTK_LIST_STORE(model), &iter, 0, pi->name(), -1);
    }
    GtkWidget* vbox = gtk_vbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);
    GtkWidget* label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
    g_object_set_data(G_OBJECT(dialog), "description", label);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
  }

  {
    GtkWidget* vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox, gtk_label_new("Security"));
    GtkWidget* checkbutton;
    checkbutton = gtk_check_button_new_with_label("Require password for local apps");
    gtk_box_pack_start(GTK_BOX(vbox), checkbutton, FALSE, FALSE, 0);
    checkbutton = gtk_check_button_new_with_label("Require password for LAN apps");
    gtk_box_pack_start(GTK_BOX(vbox), checkbutton, FALSE, FALSE, 0);
  }
  
  gtk_widget_set_size_request(dialog, 450, 300);
  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static void
about_click(GtkWidget* widget, gpointer user_data) {
  const gchar* authors[2] = {"mattn", NULL};
  gchar* contents = NULL;
  gchar* utf8 = NULL;
  GdkPixbuf* logo = NULL;
  GtkWidget* dialog;
  dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(dialog), "Growl For Linux");
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);
  if (g_file_get_contents("COPYING", &contents, NULL, NULL)) {
    utf8 = g_locale_to_utf8(contents, -1, NULL, NULL, NULL);
    g_free(contents);
    contents = NULL;
    gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(dialog), utf8);
    g_free(utf8);
    utf8 = NULL;
  }
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "http://mattn.kaoriya.net/");
  logo = gdk_pixbuf_new_from_file("./data/growl4linux.jpg", NULL);
  gtk_about_dialog_set_logo (GTK_ABOUT_DIALOG(dialog), logo);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static void
exit_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  gtk_main_quit();
}

static gpointer
recv_thread(gpointer data) {
  int sock = (int) data;
  int need_to_show = 0;

  NOTIFICATION_INFO* ni = g_new0(NOTIFICATION_INFO, 1);
  if (!ni) {
    perror("g_new0");
  }

  char* display = NULL;
  char* ptr;
  int r = readall(sock, &ptr);
  char* top = ptr;
  char* end = ptr + r;
  if (!strncmp(ptr, "GNTP/1.0 ", 9)) {
    ptr += 9;
    if (!strncmp(ptr, "REGISTER ", 9)) {
      ptr += 9;
      // TODO: register
    } else
    if (!strncmp(ptr, "NOTIFY ", 7)) {
      ptr += 7;
      char* data = NULL;
      if (!strncmp(ptr, "NONE", 4) && strchr("\n ", *(ptr+5))) {
        ptr = strchr(ptr, '\r');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        if (*ptr != '\n') goto leave;
        *ptr++ = 0;
        data = (char*) calloc(r-(ptr-top)-4+1, 1);
        if (!data) goto leave;
        memcpy(data, ptr, r-(ptr-top)-4);
      } else {
        if (strncmp(ptr, "AES:", 4) &&
            strncmp(ptr, "DES:", 4) &&
            strncmp(ptr, "3DES:", 5)) goto leave;

        char* crypt_algorythm = ptr;
        ptr = strchr(ptr, ':');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        char* iv;
        iv = ptr;
        ptr = strchr(ptr, ' ');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;

        if (strncmp(ptr, "MD5:", 4) &&
            strncmp(ptr, "SHA1:", 5) &&
            strncmp(ptr, "SHA256:", 7)) goto leave;

        char* hash_algorythm = ptr;
        ptr = strchr(ptr, ':');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        char* key = ptr;
        ptr = strchr(ptr, '.');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        char* salt = ptr;

        ptr = strchr(ptr, '\r');
        if (!ptr || ptr > end) goto leave;
        *ptr++ = 0;
        if (*ptr != '\n') goto leave;
        *ptr++ = 0;

        int n, keylen, saltlen, ivlen;

        char hex[3];
        hex[2] = 0;
        saltlen = strlen(salt) / 2;
        for (n = 0; n < saltlen; n++)
          salt[n] = unhex(salt[n * 2]) * 16 + unhex(salt[n * 2 + 1]);
        keylen = strlen(key) / 2;
        for (n = 0; n < keylen; n++)
          key[n] = unhex(key[n * 2]) * 16 + unhex(key[n * 2 + 1]);
        ivlen = strlen(iv) / 2;
        for (n = 0; n < ivlen; n++)
          iv[n] = unhex(iv[n * 2]) * 16 + unhex(iv[n * 2 + 1]);

        char digest[32] = {0};
        memset(digest, 0, sizeof(digest));

        if (!strcmp(hash_algorythm, "MD5")) {
          MD5_CTX ctx;
          MD5_Init(&ctx);
          MD5_Update(&ctx, password, strlen(password));
          MD5_Update(&ctx, salt, saltlen);
          MD5_Final((unsigned char*) digest, &ctx);
        }
        if (!strcmp(hash_algorythm, "SHA1")) {
          SHA_CTX ctx;
          SHA1_Init(&ctx);
          SHA1_Update(&ctx, password, strlen(password));
          SHA1_Update(&ctx, salt, saltlen);
          SHA1_Final((unsigned char*) digest, &ctx);
        }
        if (!strcmp(hash_algorythm, "SHA256")) {
          SHA256_CTX ctx;
          SHA256_Init(&ctx);
          SHA256_Update(&ctx, password, strlen(password));
          SHA256_Update(&ctx, salt, saltlen);
          SHA256_Final((unsigned char*) digest, &ctx);
        }

        data = (char*) calloc(r, 1);
        if (!data) goto leave;
        if (!strcmp(crypt_algorythm, "AES")) {
          AES_KEY aeskey;
          AES_set_decrypt_key((unsigned char*) digest, 24 * 8, &aeskey);
          AES_cbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
              r-(ptr-top)-6, &aeskey, (unsigned char*) iv, AES_DECRYPT);
        }
        if (!strcmp(crypt_algorythm, "DES")) {
          des_key_schedule schedule;
          DES_set_key_unchecked((const_DES_cblock*) &digest, &schedule);
          DES_ncbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
              r-(ptr-top)-6, &schedule, (const_DES_cblock*) &iv, DES_DECRYPT);
        }
        if (!strcmp(crypt_algorythm, "3DES")) {
          char key1[8], key2[8], key3[8];
          memcpy(key1, digest+ 0, 8);
          memcpy(key2, digest+ 8, 8);
          memcpy(key3, digest+16, 8);
          des_key_schedule schedule1, schedule2, schedule3;
          DES_set_key_unchecked((const_DES_cblock*) &key1, &schedule1);
          DES_set_key_unchecked((const_DES_cblock*) &key2, &schedule2);
          DES_set_key_unchecked((const_DES_cblock*) &key3, &schedule3);
          des_ede3_cbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
              r-(ptr-top)-6, schedule1, schedule2, schedule3,
              (const_DES_cblock*) &iv, DES_DECRYPT);
        }
      }

      ptr = data;
      while (*ptr) {
        char* line = ptr;
        ptr = strchr(ptr, '\r');
        if (!ptr) goto leave;
        *ptr++ = 0;
        if (*ptr != '\n') goto leave;
        *ptr++ = 0;
        if (!strncmp(line, "Notification-Title:", 19)) {
          line += 20;
          while(isspace(*line)) line++;
          ni->title = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Text:", 18)) {
          line += 19;
          while(isspace(*line)) line++;
          ni->text = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Icon:", 18)) {
          line += 19;
          while(isspace(*line)) line++;
          ni->icon = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Callback-Target:", 29)) {
          line += 30;
          while(isspace(*line)) line++;
          ni->url = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Display-Name:", 26)) {
          line += 27;
          while(isspace(*line)) line++;
          display = g_strdup(line);
        }
      }

      if (ni->title && ni->text)
        need_to_show = 1;
      else {
        ptr = "GNTP/1.0 -ERROR Invalid data\r\n"
            "Error-Description: Invalid data\r\n\r\n";
        send(sock, ptr, strlen(ptr), 0);
      }
      free(data);
    }
    ptr = "GNTP/1.0 OK\r\n\r\n";
    send(sock, ptr, strlen(ptr), 0);
  } else {
    ptr = "GNTP/1.0 -ERROR Invalid command\r\n\r\n";
    send(sock, ptr, strlen(ptr), 0);
  }
  free(top);
  closesocket(sock);
  if (need_to_show) {
    PLUGIN_INFO* cp = current_plugin;
    int i, len = g_list_length(plugin_list);
    for (i = 0; i < len; i++) {
      PLUGIN_INFO* pi = (PLUGIN_INFO*) g_list_nth_data(plugin_list, i);
      if (!g_strcasecmp(pi->name(), display)) {
        cp = pi;
        break;
      }
    }
    g_idle_add((GSourceFunc) cp->show, ni); // call once
  } else {
    g_free(ni->title);
    g_free(ni->text);
    g_free(ni->icon);
    g_free(ni->url);
    g_free(ni);
  }
  g_free(display);
  return NULL;

leave:
  free(top);
  closesocket(sock);
  free(ni);
  return NULL;
}

static void
signal_handler(int num) {
  gtk_main_quit();
}

/*
static GdkPixbuf*
disabled_pixbuf(GdkPixbuf *pixbuf) {
  GdkPixbuf* gray = gdk_pixbuf_copy(pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(gray);
  int pixstride = gdk_pixbuf_get_has_alpha(gray) ? 4 : 3;
  guchar* pixels = gdk_pixbuf_get_pixels(gray);
  int n_rows = gdk_pixbuf_get_height(gray);
  int width = gdk_pixbuf_get_width(gray);
  int row = 0;

  while (row < n_rows) {
    guchar *p = pixels + row * rowstride;
    guchar *end = p + (pixstride * width);
    while (p != end) {
#define INTENSITY(r, g, b) ((r) * 0.30 + (g) * 0.59 + (b) * 0.11)
      double v = INTENSITY(p[0], p[1], p[2]);
#undef INTENSITY
      p[0] = (guchar) v;
      p[1] = (guchar) v;
      p[2] = (guchar) v;
      p += pixstride;
    }
    row++;
  }
  return gray;
}
*/


static void
create_ui() {
  GtkWidget* menu;
  GtkWidget* menu_item;

  // TODO: absolute path
  status_icon = gtk_status_icon_new_from_file("./data/icon.png");
  gtk_status_icon_set_tooltip(status_icon, "Growl");
  gtk_status_icon_set_visible(status_icon, TRUE);
  menu = gtk_menu_new();
  g_signal_connect(GTK_STATUS_ICON(status_icon), "popup-menu", G_CALLBACK(status_icon_popup), menu);

  menu_item = gtk_menu_item_new_with_label("Settings");
  g_signal_connect(G_OBJECT(menu_item), "activate", G_CALLBACK(settings_clicked), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
  menu_item = gtk_menu_item_new_with_label("About");
  g_signal_connect(G_OBJECT(menu_item), "activate", G_CALLBACK(about_click), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  menu_item = gtk_menu_item_new_with_label("Exit");
  g_signal_connect(G_OBJECT(menu_item), "activate", G_CALLBACK(exit_clicked), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
  gtk_widget_show_all(menu);
}

static void
destroy_ui() {
  gtk_status_icon_set_visible(GTK_STATUS_ICON(status_icon), FALSE);
}

static void
open_config() {
  char* error;
  gchar* confdir = (gchar*) g_get_user_config_dir();
  confdir = g_build_path(G_DIR_SEPARATOR_S, confdir, "gol", NULL);
  g_mkdir_with_parents(confdir, 0700);
  gchar* confdb = g_build_filename(confdir, "config.db", NULL);
  int rc;
  if (!g_file_test(confdb, G_FILE_TEST_EXISTS)) {
    char* sqls[] = {
      "create table config(key text not null primary key, value text not null)",
      "create table notification(name text not null primary key, enable bool not null, display text not null, sticky bool not null)",
      NULL
    };
    char** sql = sqls;
    rc = sqlite3_open(confdb, &db);
    while (*sql) {
      rc = sqlite3_exec(db, *sql, 0, 0, &error);
      sql++;
    }
    sqlite3_close(db);
  }
  rc = sqlite3_open(confdb, &db);

  const char* sql = "select value from config where key = 'password'";
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare(db, sql, strlen(sql), &stmt, NULL);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    password = g_strdup((char*) sqlite3_column_text(stmt, 0));
  } else {
    password = g_strdup("");
  }
}

static void
close_config() {
  sqlite3_close(db);
}

static void
load_display_plugin_list(const gchar* path) {
  GDir *dir;
  const gchar *filename;
  dir = g_dir_open(path, 0, NULL);

  current_plugin = NULL;
  gchar* name = g_strconcat("libdefault.", G_MODULE_SUFFIX, NULL);
  while ((filename = g_dir_read_name(dir))) {
    if (!g_str_has_suffix(filename, G_MODULE_SUFFIX))
      continue;

    gchar* fullpath = g_build_filename(path, filename, NULL);
    GModule* handle = g_module_open(fullpath, G_MODULE_BIND_LAZY);
    if (!handle) {
      g_free(fullpath);
      continue;
    }
    PLUGIN_INFO* pi = g_new0(PLUGIN_INFO, 1);
    g_module_symbol(handle, "notification_show", (void**) &pi->show);
    g_module_symbol(handle, "notification_init", (void**) &pi->init);
    g_module_symbol(handle, "notification_term", (void**) &pi->term);
    g_module_symbol(handle, "notification_name", (void**) &pi->name);
    g_module_symbol(handle, "notification_description", (void**) &pi->description);
    plugin_list = g_list_append(plugin_list, pi);
    if (pi && pi->name && !g_strcasecmp(pi->name(), name)) current_plugin = pi;
  }
  g_free(name);
  g_dir_close(dir);

  if (!current_plugin) current_plugin = g_list_nth_data(plugin_list, 0);
  current_plugin->init();
}

static gboolean
socket_accepted(GIOChannel* source, GIOCondition condition, gpointer data) {
  int fd = g_io_channel_unix_get_fd(source);
  int sock;
  struct sockaddr_in client;
  int client_len = sizeof(client);
  memset(&client, 0, sizeof(client));
  if ((sock = accept(fd, (struct sockaddr *) &client, (socklen_t *) &client_len)) < 0) {
    perror("accept");
    return TRUE;
  }
#ifdef G_THREADS_ENABLED
  g_thread_create(recv_thread, (gpointer) sock, FALSE, NULL);
#else
  recv_thread((gpointer) sock);
#endif
  return TRUE;
}

int
main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

  gchar* program = g_find_program_in_path(argv[0]);
  exepath = g_path_get_dirname(program);
  g_free(program);

#ifdef G_THREADS_ENABLED
  g_thread_init(NULL);
#endif

  gtk_init(&argc, &argv);

  create_ui();

  open_config();

  gchar* path = g_build_filename(DATADIR, "display", NULL);
  load_display_plugin_list(path);
  g_free(path);

  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  int fd;
  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    exit(1);
  }

  sockopt_t sockopt;
  sockopt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    exit(1);
  }
  sockopt = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    exit(1);
  }

  struct sockaddr_in server_addr;
  memset((char *) &server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(23053);

  if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    exit(1);
  }

  if (listen(fd, SOMAXCONN) < 0) {
    perror("listen");
    closesocket(fd);
    exit(1);
  }

  fd_set fdset;
  FD_SET(fd, &fdset);
  GIOChannel* channel = g_io_channel_unix_new(fd);
  g_io_add_watch(channel, G_IO_IN | G_IO_ERR, socket_accepted, NULL);
  g_io_channel_unref(channel);

  gtk_main();

  close_config();

  destroy_ui();

#ifdef _WIN32
  WSACleanup();
#endif

  return 0;
}

// vim:set et sw=2 ts=2 ai:
