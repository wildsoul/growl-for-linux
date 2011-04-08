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
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <curl/curl.h>
#ifdef _WIN32
# include <io.h>
#endif

#ifdef _WIN32
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
#endif

#define REQUEST_TIMEOUT            (10)

typedef struct {
  gchar* title;
  gchar* message;
  gchar* icon;
  gchar* url;
  gint timeout;

  GtkWidget* popup;
  GdkRectangle rect;
  gint width;
  gint height;
  gint offset;

} POPUP_INFO;

GList* popup_list = NULL;

typedef struct {
  char* data;     // response data from server
  size_t size;    // response size of data
} MEMFILE;

static MEMFILE*
memfopen() {
  MEMFILE* mf = (MEMFILE*) malloc(sizeof(MEMFILE));
  if (mf) {
    mf->data = NULL;
    mf->size = 0;
  }
  return mf;
}

static void
memfclose(MEMFILE* mf) {
  if (mf->data) free(mf->data);
  free(mf);
}

static size_t
memfwrite(char* ptr, size_t size, size_t nmemb, void* stream) {
  MEMFILE* mf = (MEMFILE*) stream;
  int block = size * nmemb;
  if (!mf) return block; // through
  if (!mf->data)
    mf->data = (char*) malloc(block);
  else
    mf->data = (char*) realloc(mf->data, mf->size + block);
  if (mf->data) {
    memcpy(mf->data + mf->size, ptr, block);
    mf->size += block;
  }
  return block;
}

static char*
memfstrdup(MEMFILE* mf) {
  char* buf;
  if (mf->size == 0) return NULL;
  buf = (char*) malloc(mf->size + 1);
  memcpy(buf, mf->data, mf->size);
  buf[mf->size] = 0;
  return buf;
}

static char*
get_http_header_alloc(const char* ptr, const char* key) {
  const char* tmp = ptr;

  while (*ptr) {
    tmp = strpbrk(ptr, "\r\n");
    if (!tmp) break;
    if (!strncasecmp(ptr, key, strlen(key)) && *(ptr + strlen(key)) == ':') {
      size_t len;
      char* val;
      const char* top = ptr + strlen(key) + 1;
      while (*top && isspace(*top)) top++;
      if (!*top) return NULL;
      len = tmp - top + 1;
      val = malloc(len);
      memset(val, 0, len);
      strncpy(val, top, len-1);
      return val;
    }
    ptr = tmp + 1;
  }
  return NULL;
}

static GdkPixbuf*
url2pixbuf(const char* url, GError** error) {
  GdkPixbuf* pixbuf = NULL;
  GdkPixbufLoader* loader = NULL;
  GError* _error = NULL;

  if (!strncmp(url, "file:///", 8) || g_file_test(url, G_FILE_TEST_EXISTS)) {
    gchar* newurl = g_filename_from_uri(url, NULL, NULL);
    pixbuf = gdk_pixbuf_new_from_file(newurl ? newurl : url, &_error);
  } else {
    CURL* curl = NULL;
    MEMFILE* mbody;
    MEMFILE* mhead;
    char* head;
    char* body;
    unsigned long size;
    CURLcode res = CURLE_FAILED_INIT;

    curl = curl_easy_init();
    if (!curl) return NULL;

    mbody = memfopen();
    mhead = memfopen();

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, REQUEST_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memfwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, mbody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, memfwrite);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, mhead);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    head = memfstrdup(mhead);
    memfclose(mhead);
    body = memfstrdup(mbody);
    size = mbody->size;
    memfclose(mbody);

    if (res == CURLE_OK) {
      char* ctype;
      char* csize;
      ctype = get_http_header_alloc(head, "Content-Type");
      csize = get_http_header_alloc(head, "Content-Length");

#ifdef _WIN32
      if (ctype &&
          (!strcmp(ctype, "image/jpeg") || !strcmp(ctype, "image/gif"))) {
        char temp_path[MAX_PATH];
        char temp_filename[MAX_PATH];
        FILE* fp;
        GetTempPath(sizeof(temp_path), temp_path);
        GetTempFileName(temp_path, "growl-for-linux-", 0, temp_filename);
        fp = fopen(temp_filename, "wb");
        if (fp) {
          fwrite(body, size, 1, fp);
          fclose(fp);
        }
        pixbuf = gdk_pixbuf_new_from_file(temp_filename, NULL);
        DeleteFile(temp_filename);
      } else
#endif
      {
        if (ctype)
          loader =
            (GdkPixbufLoader*) gdk_pixbuf_loader_new_with_mime_type(ctype,
                error);
        if (csize)
          size = atol(csize);
        if (!loader) loader = gdk_pixbuf_loader_new();
        if (body && gdk_pixbuf_loader_write(loader, (const guchar*) body,
              size, &_error)) {
          pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        }
      }
      if (ctype) free(ctype);
      if (csize) free(csize);
      if (loader) gdk_pixbuf_loader_close(loader, NULL);
    } else {
      _error = g_error_new_literal(G_FILE_ERROR, res,
          curl_easy_strerror(res));
    }

    free(head);
    free(body);
  }

  /* cleanup callback data */
  if (error && _error) *error = _error;
  return pixbuf;
}

static int
open_url(const gchar* url) {
  int r = 0;
#if defined(_WIN32)
  r = (int) ShellExecute(NULL, "open", url, NULL, NULL, SW_SHOW);
#elif defined(MACOSX)
  gchar* command = g_strdup_printf("open '%s'", url);
  g_spawn_command_line_async(command, NULL);
  g_free(command);
#else
  gchar* command = g_strdup_printf("xdg-open '%s'", url);
  g_spawn_command_line_async(command, NULL);
  g_free(command);
#endif
  return r;
}

static void
popup_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  POPUP_INFO* pi = (POPUP_INFO*) user_data;
  if (pi->url) open_url(pi->url);
}

static gboolean
popup_animation_func(gpointer data) {
  POPUP_INFO* pi = (POPUP_INFO*) data;

  //printf("%d\n", pi->timeout);
  if (pi->timeout-- < 0) {
    gtk_widget_destroy(pi->popup);
    return FALSE;
  }

  if (pi->offset < pi->height) {
    pi->offset += 2;

    gtk_window_resize(GTK_WINDOW(pi->popup), pi->width, pi->offset);
    gtk_window_move(GTK_WINDOW(pi->popup),
        pi->rect.x + pi->rect.width - pi->width,
        pi->rect.y + pi->rect.height - pi->offset);
  }
  return TRUE;
}

static void
popup_show(POPUP_INFO* pi) {
  //GdkWindow *root_window = gdk_get_default_root_window();
  GdkColor color;
  gdk_color_parse ("white", &color);
  GtkWidget* fixed;
  GtkWidget* vbox;
  GtkWidget* hbox;
  GtkWidget* label;
  GtkWidget* image;
  GdkPixbuf* pixbuf;
  GdkScreen* screen;
  int monitor_num;

  screen = gdk_screen_get_default();
  monitor_num = gdk_screen_get_primary_monitor(screen);
  gdk_screen_get_monitor_geometry(screen, monitor_num, &pi->rect);

  pi->popup = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_window_set_title(GTK_WINDOW(pi->popup), "growl-for-linux");
  gtk_window_set_resizable(GTK_WINDOW(pi->popup), TRUE);
  gtk_window_set_decorated(GTK_WINDOW(pi->popup), FALSE);
  gtk_window_set_keep_above(GTK_WINDOW(pi->popup), TRUE);
  gtk_window_stick(GTK_WINDOW(pi->popup));
  gtk_window_set_opacity(GTK_WINDOW(pi->popup), 0.8);
  gtk_widget_modify_bg(pi->popup, GTK_STATE_NORMAL, &color);

  fixed = gtk_fixed_new();
  gtk_container_set_border_width(GTK_CONTAINER(fixed), 10);
  gtk_container_add(GTK_CONTAINER(pi->popup), fixed);

  vbox = gtk_vbox_new(FALSE, 5);
  gtk_container_add(GTK_CONTAINER(fixed), vbox);

  hbox = gtk_hbox_new(FALSE, 5);
  pixbuf = url2pixbuf(pi->icon, NULL);
  if (pixbuf) {
    GdkPixbuf* tmp = gdk_pixbuf_scale_simple(pixbuf, 32, 32, GDK_INTERP_TILES);
    if (tmp) pixbuf = tmp;
  }
  image = gtk_image_new_from_pixbuf(pixbuf);
  gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);

  label = gtk_label_new(pi->title);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

  label = gtk_label_new(pi->message);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
  gtk_widget_set_size_request(label, 160, 120);
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

  gtk_window_resize(GTK_WINDOW(pi->popup), 180, 160);
  //gtk_widget_set_size_request(pi->popup, 180, 160);
  gtk_window_get_size(GTK_WINDOW(pi->popup), &pi->width, &pi->height);

  g_signal_connect(G_OBJECT(pi->popup), "button-press-event", G_CALLBACK(popup_clicked), pi);

  pi->offset = 0;
  pi->timeout = 200;

  gtk_window_move(GTK_WINDOW(pi->popup),
      pi->rect.x + pi->rect.width - pi->width,
      pi->rect.y + pi->rect.height);
  gtk_widget_show_all(pi->popup);

  g_timeout_add(10, popup_animation_func, pi);
}

int
main(int argc, char* argv[]) {
  gtk_init(&argc, &argv);
  POPUP_INFO* pi = (POPUP_INFO*) malloc(sizeof(POPUP_INFO));
  pi->title = "はろー";
  pi->message = "わーるどーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーー";
  pi->icon = "http://mattn.kaoriya.net/images/logo.png";
  pi->url = "http://mattn.kaoriya.net/";
  popup_show(pi);
  gtk_main();
  return 0;
}

// vim:set et sw=2 ts=2 ai:
