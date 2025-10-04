#include "splashlib.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
  const char *name;
  int *indices;
  int count;
  gboolean loop_at_end;
} ComboSeq;

static void free_combos(ComboSeq *combos, int count) {
  if (!combos) return;
  for (int i = 0; i < count; ++i) {
    g_free(combos[i].indices);
  }
  g_free(combos);
}

typedef struct {
  Splash *splash;
  SplashSeq *sequences;
  int sequence_count;
  ComboSeq *combos;
  int combo_count;
  gboolean started;
  gboolean combo_loop_full;
  GMainLoop *loop;
} AppCtx;

static gboolean set_stdin_nonblock(void) {
  struct termios t; if (tcgetattr(STDIN_FILENO, &t)) return FALSE;
  t.c_lflag &= ~(ICANON | ECHO);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &t)) return FALSE;
  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  return fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0;
}

static gchar *json_escape(const char *in) {
  if (!in) return g_strdup("");
  GString *out = g_string_new(NULL);
  for (const unsigned char *p = (const unsigned char*)in; *p; ++p) {
    switch (*p) {
      case '\\': g_string_append(out, "\\\\"); break;
      case '\"': g_string_append(out, "\\\""); break;
      case '\b': g_string_append(out, "\\b"); break;
      case '\f': g_string_append(out, "\\f"); break;
      case '\n': g_string_append(out, "\\n"); break;
      case '\r': g_string_append(out, "\\r"); break;
      case '\t': g_string_append(out, "\\t"); break;
      default:
        if (*p < 0x20) {
          g_string_append_printf(out, "\\u%04x", *p);
        } else {
          g_string_append_c(out, (char)*p);
        }
    }
  }
  return g_string_free(out, FALSE);
}

static ComboSeq *find_combo_by_name(AppCtx *ctx, const char *name) {
  if (!ctx || !name) return NULL;
  for (int i = 0; i < ctx->combo_count; ++i) {
    if (g_strcmp0(ctx->combos[i].name, name) == 0) {
      return &ctx->combos[i];
    }
  }
  return NULL;
}

static gboolean send_http_response(GOutputStream *out,
                                   int status,
                                   const char *reason,
                                   const char *content_type,
                                   const char *body) {
  if (!content_type) content_type = "text/plain";
  if (!body) body = "";
  gsize body_len = strlen(body);
  GString *resp = g_string_new(NULL);
  g_string_append_printf(resp, "HTTP/1.1 %d %s\r\n", status, reason);
  g_string_append_printf(resp, "Content-Type: %s\r\n", content_type);
  g_string_append_printf(resp, "Content-Length: %" G_GSIZE_FORMAT "\r\n", body_len);
  g_string_append(resp, "Connection: close\r\n\r\n");
  g_string_append_len(resp, body, body_len);
  gsize written = 0;
  GError *error = NULL;
  gboolean ok = g_output_stream_write_all(out, resp->str, resp->len,
                                          &written, NULL, &error);
  if (!ok && error) {
    fprintf(stderr, "HTTP response write failed: %s\n", error->message);
    g_error_free(error);
  }
  g_string_free(resp, TRUE);
  return ok;
}

static gboolean handle_http_path(AppCtx *ctx,
                                 const char *path,
                                 GOutputStream *out) {
  if (!g_strcmp0(path, "/request/start")) {
    if (ctx->started) {
      return send_http_response(out, 200, "OK",
                                "application/json",
                                "{\"status\":\"already_running\"}");
    }
    if (!splash_start(ctx->splash)) {
      return send_http_response(out, 500, "Internal Server Error",
                                "application/json",
                                "{\"status\":\"error\",\"message\":\"failed_to_start\"}");
    }
    ctx->started = TRUE;
    return send_http_response(out, 200, "OK",
                              "application/json",
                              "{\"status\":\"started\"}");
  }

  if (!g_strcmp0(path, "/request/stop")) {
    if (!ctx->started) {
      return send_http_response(out, 200, "OK",
                                "application/json",
                                "{\"status\":\"already_stopped\"}");
    }
    splash_stop(ctx->splash);
    ctx->started = FALSE;
    return send_http_response(out, 200, "OK",
                              "application/json",
                              "{\"status\":\"stopped\"}");
  }

  if (!g_strcmp0(path, "/request/list")) {
    GString *body = g_string_new("{\"sequences\":[");
    for (int i = 0; i < ctx->sequence_count; ++i) {
      if (i > 0) g_string_append(body, ",");
      gchar *escaped = json_escape(ctx->sequences[i].name);
      g_string_append(body, "\"");
      g_string_append(body, escaped);
      g_string_append(body, "\"");
      g_free(escaped);
    }
    g_string_append(body, "],\"combos\":[");
    for (int i = 0; i < ctx->combo_count; ++i) {
      if (i > 0) g_string_append(body, ",");
      gchar *escaped = json_escape(ctx->combos[i].name);
      g_string_append(body, "{\"name\":\"");
      g_string_append(body, escaped);
      g_string_append(body, "\",\"order\":[");
      g_free(escaped);
      for (int j = 0; j < ctx->combos[i].count; ++j) {
        if (j > 0) g_string_append(body, ",");
        const char *part_name = NULL;
        int idx = ctx->combos[i].indices[j];
        if (idx >= 0 && idx < ctx->sequence_count) {
          part_name = ctx->sequences[idx].name;
        }
        gchar *part_escaped = json_escape(part_name ? part_name : "");
        g_string_append(body, "\"");
        g_string_append(body, part_escaped);
        g_string_append(body, "\"");
        g_free(part_escaped);
      }
      g_string_append(body, "]");
      g_string_append(body, ",\"loop_at_end\":");
      g_string_append(body, ctx->combos[i].loop_at_end ? "true" : "false");
      g_string_append(body, "}");
    }
    g_string_append(body, "]}");
    gboolean ok = send_http_response(out, 200, "OK",
                                     "application/json",
                                     body->str);
    g_string_free(body, TRUE);
    return ok;
  }

  const char *enqueue_prefix = "/request/enqueue/";
  if (g_str_has_prefix(path, enqueue_prefix)) {
    const char *raw_name = path + strlen(enqueue_prefix);
    gchar *decoded = g_uri_unescape_string(raw_name, NULL);
    gboolean ok = FALSE;
    if (decoded && decoded[0] != '\0') {
      int idx = splash_find_index_by_name(ctx->splash, decoded);
      if (idx >= 0) {
        if (splash_enqueue_next_by_index(ctx->splash, idx)) {
          splash_set_repeat_order(ctx->splash, NULL, 0);
          gchar *escaped = json_escape(decoded);
          GString *body = g_string_new("{\"status\":\"queued\",\"name\":\"");
          g_string_append(body, escaped);
          g_string_append(body, "\"}");
          ok = send_http_response(out, 200, "OK",
                                  "application/json",
                                  body->str);
          g_string_free(body, TRUE);
          g_free(escaped);
        } else {
          ok = send_http_response(out, 409, "Conflict",
                                  "application/json",
                                  "{\"status\":\"queue_full\"}");
        }
      } else {
        ComboSeq *combo = find_combo_by_name(ctx, decoded);
        if (combo && combo->count > 0) {
          if (splash_enqueue_next_many(ctx->splash, combo->indices, combo->count)) {
            if (combo->loop_at_end) {
              if (ctx->combo_loop_full) {
                splash_set_repeat_order(ctx->splash, combo->indices, combo->count);
              } else {
                splash_set_repeat_order(ctx->splash,
                                        &combo->indices[combo->count - 1], 1);
              }
            } else {
              splash_set_repeat_order(ctx->splash, NULL, 0);
            }
            gchar *escaped = json_escape(decoded);
            GString *body = g_string_new("{\"status\":\"queued_combo\",\"name\":\"");
            g_string_append(body, escaped);
            g_string_append_printf(body, "\",\"length\":%d}", combo->count);
            ok = send_http_response(out, 200, "OK",
                                    "application/json",
                                    body->str);
            g_string_free(body, TRUE);
            g_free(escaped);
          } else {
            ok = send_http_response(out, 409, "Conflict",
                                    "application/json",
                                    "{\"status\":\"queue_full\"}");
          }
        } else {
          gchar *escaped = json_escape(decoded);
          GString *body = g_string_new("{\"status\":\"not_found\",\"name\":\"");
          g_string_append(body, escaped);
          g_string_append(body, "\"}");
          ok = send_http_response(out, 404, "Not Found",
                                  "application/json",
                                  body->str);
          g_string_free(body, TRUE);
          g_free(escaped);
        }
      }
    } else {
      ok = send_http_response(out, 400, "Bad Request",
                              "application/json",
                              "{\"status\":\"invalid_name\"}");
    }
    g_free(decoded);
    return ok;
  }

  return send_http_response(out, 404, "Not Found",
                            "application/json",
                            "{\"status\":\"unknown_request\"}");
}

static gboolean on_http_client(GSocketService *service,
                               GSocketConnection *connection,
                               GObject *source_object,
                               gpointer user_data) {
  (void)service;
  (void)source_object;
  AppCtx *ctx = (AppCtx *)user_data;
  GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  char buffer[2048];
  GError *error = NULL;
  gssize n = g_input_stream_read(in, buffer, sizeof(buffer) - 1, NULL, &error);
  if (n <= 0) {
    if (error) {
      fprintf(stderr, "HTTP read failed: %s\n", error->message);
      g_error_free(error);
    }
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
  }
  buffer[n] = '\0';

  char method[16] = {0};
  char path[1024] = {0};
  if (sscanf(buffer, "%15s %1023s", method, path) != 2) {
    send_http_response(out, 400, "Bad Request",
                       "application/json",
                       "{\"status\":\"bad_request\"}");
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
  }

  if (g_strcmp0(method, "GET") != 0) {
    send_http_response(out, 405, "Method Not Allowed",
                       "application/json",
                       "{\"status\":\"method_not_allowed\"}");
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
  }

  char *query = strchr(path, '?');
  if (query) *query = '\0';

  handle_http_path(ctx, path, out);
  g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
  return TRUE;
}

static gboolean on_stdin_ready(GIOChannel *source, GIOCondition condition, gpointer user_data) {
  (void)source;
  (void)condition;
  AppCtx *ctx = (AppCtx *)user_data;
  for (;;) {
    char ch;
    ssize_t r = read(STDIN_FILENO, &ch, 1);
    if (r == 0) break;
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      perror("read");
      break;
    }
    if (ch == 'q') {
      if (ctx->loop) g_main_loop_quit(ctx->loop);
      break;
    } else if (ch == 'c') {
      splash_clear_next(ctx->splash);
    } else if (ch == 's') {
      if (!ctx->started && splash_start(ctx->splash)) {
        ctx->started = TRUE;
      }
    } else if (ch == 'x') {
      if (ctx->started) {
        splash_stop(ctx->splash);
        ctx->started = FALSE;
      }
    } else if (ch >= '1' && ch <= '9') {
      int idx = ch - '1';
      if (idx < ctx->sequence_count) {
        if (splash_enqueue_next_by_index(ctx->splash, idx)) {
          splash_set_repeat_order(ctx->splash, NULL, 0);
        }
      }
    }
  }
  return G_SOURCE_CONTINUE;
}

static void on_evt(SplashEventType type, int a, int b, const char *msg, void *user){
  AppCtx *ctx = (AppCtx*)user;
  switch(type){
    case SPLASH_EVT_STARTED:
      if (ctx) ctx->started = TRUE;
      fprintf(stderr, "[evt] started\n");
      break;
    case SPLASH_EVT_STOPPED:
      if (ctx) ctx->started = FALSE;
      fprintf(stderr, "[evt] stopped\n");
      break;
    case SPLASH_EVT_SWITCHED_AT_BOUNDARY:
      fprintf(stderr, "[evt] switched at boundary: %d -> %d\n", a, b); break;
    case SPLASH_EVT_QUEUED_NEXT:
      fprintf(stderr, "[evt] queued next idx=%d\n", a); break;
    case SPLASH_EVT_CLEARED_QUEUE:
      fprintf(stderr, "[evt] cleared next\n"); break;
    case SPLASH_EVT_ERROR:
      fprintf(stderr, "[evt] ERROR: %s\n", msg?msg:"?"); break;
  }
}

#define SEQ_GROUP_PREFIX "sequence"

static void usage(const char *p){
  fprintf(stderr,
    "Usage:\n"
    "  %s [--cli] [--http-port=PORT] <config.ini>\n\n"
    "The configuration file must contain a [stream] group with keys:\n"
    "  input=/path/to/file.h265\n"
    "  fps=30.0\n"
    "  host=127.0.0.1\n"
    "  port=5600\n"
    "and one or more [sequence NAME] groups. Define raw clips with:\n"
    "  start=BEGIN_FRAME\n"
    "  end=END_FRAME\n"
    "or build combo playlists with:\n"
    "  order=seqA,seqB,...   (references previously defined sequences)\n"
    "  loop_at_end=true|false (optional; enables full-combo repeats in 'entire' mode)\n"
    "Optionally add a [control] group with:\n"
    "  port=8081   (HTTP control port; defaults to 8081 if omitted)\n\n"
    "  combo_loop_mode=final|entire (default=final).\n\n"
    "Options:\n"
    "  --cli           Enable interactive stdin controls (1-9 enqueue, c=clear, s=start, x=stop, q=quit).\n"
    "  --http-port=NN  Override HTTP control port (default is config [control] port or 8081).\n",
    p);
}

typedef struct {
  gchar *name;
  GPtrArray *parts; // array of gchar* (owned)
  gboolean loop_at_end;
} PendingCombo;

static void pending_combo_free(PendingCombo *pc) {
  if (!pc) return;
  if (pc->parts) {
    for (guint i = 0; i < pc->parts->len; ++i) {
      g_free(g_ptr_array_index(pc->parts, i));
    }
    g_ptr_array_free(pc->parts, TRUE);
  }
  g_free(pc->name);
  g_free(pc);
}

static gchar *extract_sequence_name(const gchar *group, GError **error) {
  const gsize prefix_len = strlen(SEQ_GROUP_PREFIX);
  const gchar *raw = group + prefix_len;
  while (g_ascii_isspace(*raw)) raw++;

  if (*raw == '\0') {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "Sequence group '%s' is missing a name", group);
    return NULL;
  }

  gchar *name = g_strdup(raw);
  g_strstrip(name);
  gsize name_len = strlen(name);
  if (name_len >= 2 && name[0] == '"' && name[name_len - 1] == '"') {
    memmove(name, name + 1, name_len - 2);
    name[name_len - 2] = '\0';
  }
  if (*name == '\0') {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "Sequence group '%s' resolved to an empty name", group);
    g_free(name);
    return NULL;
  }
  return name;
}

static gboolean parse_sequence_group(GKeyFile *kf, const gchar *group,
                                     GPtrArray *owned_strings,
                                     GArray *out_sequences,
                                     GError **error) {
  GError *local_error = NULL;
  gchar *name = extract_sequence_name(group, &local_error);
  if (!name) {
    if (local_error) g_propagate_error(error, local_error);
    return FALSE;
  }

  gint start = g_key_file_get_integer(kf, group, "start", &local_error);
  if (local_error) {
    g_propagate_error(error, local_error);
    g_free(name);
    return FALSE;
  }
  gint end = g_key_file_get_integer(kf, group, "end", &local_error);
  if (local_error) {
    g_propagate_error(error, local_error);
    g_free(name);
    return FALSE;
  }
  if (start > end) {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "Sequence '%s' has start (%d) after end (%d)", name, start, end);
    g_free(name);
    return FALSE;
  }

  SplashSeq seq = { name, start, end };
  g_ptr_array_add(owned_strings, name);
  g_array_append_val(out_sequences, seq);
  return TRUE;
}

static gboolean parse_combo_group(GKeyFile *kf, const gchar *group,
                                  PendingCombo **out_combo,
                                  GError **error) {
  GError *local_error = NULL;
  gchar *name = extract_sequence_name(group, &local_error);
  if (!name) {
    if (local_error) g_propagate_error(error, local_error);
    return FALSE;
  }

  gchar *order = g_key_file_get_string(kf, group, "order", &local_error);
  if (local_error) {
    g_propagate_error(error, local_error);
    g_free(name);
    return FALSE;
  }
  if (!order) {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "Combo sequence '%s' missing order", name);
    g_free(name);
    return FALSE;
  }

  gboolean loop_at_end = FALSE;
  if (g_key_file_has_key(kf, group, "loop_at_end", NULL)) {
    local_error = NULL;
    loop_at_end = g_key_file_get_boolean(kf, group, "loop_at_end", &local_error);
    if (local_error) {
      g_propagate_error(error, local_error);
      g_free(order);
      g_free(name);
      return FALSE;
    }
  }

  gchar **parts = g_strsplit(order, ",", -1);
  g_free(order);
  if (!parts) {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "Combo sequence '%s' has invalid order", name);
    g_free(name);
    return FALSE;
  }

  GPtrArray *part_array = g_ptr_array_new();
  for (gchar **p = parts; *p; ++p) {
    gchar *trimmed = g_strdup(*p);
    g_strstrip(trimmed);
    if (trimmed[0] == '\0') {
      g_free(trimmed);
      g_strfreev(parts);
      if (part_array) {
        for (guint i = 0; i < part_array->len; ++i) {
          g_free(g_ptr_array_index(part_array, i));
        }
        g_ptr_array_free(part_array, TRUE);
      }
      g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                  "Combo sequence '%s' contains an empty entry", name);
      g_free(name);
      return FALSE;
    }
    g_ptr_array_add(part_array, trimmed);
  }
  g_strfreev(parts);

  if (part_array->len == 0) {
    for (guint i = 0; i < part_array->len; ++i) {
      g_free(g_ptr_array_index(part_array, i));
    }
    g_ptr_array_free(part_array, TRUE);
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "Combo sequence '%s' has an empty order", name);
    g_free(name);
    return FALSE;
  }

  PendingCombo *combo = g_new0(PendingCombo, 1);
  combo->name = name;
  combo->parts = part_array;
  combo->loop_at_end = loop_at_end;
  *out_combo = combo;
  return TRUE;
}

static gboolean load_config(const char *path,
                            SplashConfig *cfg,
                            SplashSeq **seqs_out,
                            int *n_seqs_out,
                            ComboSeq **combos_out,
                            int *n_combos_out,
                            GPtrArray **owned_strings_out,
                            gboolean *combo_loop_full_out,
                            guint16 *http_port_out) {
  gboolean ok = FALSE;
  GError *error = NULL;
  ComboSeq *combo_array = NULL;
  guint combo_count = 0;
  GPtrArray *combo_defs = NULL;
  gboolean combo_loop_full = FALSE;
  GKeyFile *kf = g_key_file_new();
  if (!kf) return FALSE;

  if (combos_out) *combos_out = NULL;
  if (n_combos_out) *n_combos_out = 0;
  if (combo_loop_full_out) *combo_loop_full_out = FALSE;

  gchar *config_abs = g_canonicalize_filename(path, NULL);
  if (!config_abs) {
    g_key_file_free(kf);
    return FALSE;
  }

  gchar *config_dir = g_path_get_dirname(config_abs);
  if (!config_dir) {
    g_free(config_abs);
    g_key_file_free(kf);
    return FALSE;
  }

  if (!g_key_file_load_from_file(kf, config_abs, G_KEY_FILE_NONE, &error)) {
    fprintf(stderr, "Failed to read config '%s': %s\n", path,
            error ? error->message : "unknown error");
    if (error) g_error_free(error);
    g_free(config_abs);
    g_free(config_dir);
    g_key_file_free(kf);
    return FALSE;
  }

  GPtrArray *owned_strings = g_ptr_array_new_with_free_func(g_free);
  if (!owned_strings) {
    g_free(config_abs);
    g_free(config_dir);
    g_key_file_free(kf);
    return FALSE;
  }

  error = NULL;
  gchar *input = g_key_file_get_string(kf, "stream", "input", &error);
  if (error) {
    fprintf(stderr, "Config missing stream.input: %s\n", error->message);
    g_error_free(error);
    goto done;
  }
  gchar *resolved_input = g_canonicalize_filename(input, config_dir);
  if (!resolved_input) {
    fprintf(stderr, "Failed to resolve stream.input path '%s'\n", input);
    g_free(input);
    goto done;
  }
  g_free(input);
  if (!g_file_test(resolved_input, G_FILE_TEST_EXISTS)) {
    fprintf(stderr, "Configured input file '%s' does not exist\n",
            resolved_input);
    g_free(resolved_input);
    goto done;
  }
  g_ptr_array_add(owned_strings, resolved_input);
  cfg->input_path = resolved_input;

  error = NULL;
  cfg->fps = g_key_file_get_double(kf, "stream", "fps", &error);
  if (error) {
    fprintf(stderr, "Config missing/invalid stream.fps: %s\n", error->message);
    g_error_free(error);
    goto done;
  }

  error = NULL;
  gchar *host = g_key_file_get_string(kf, "stream", "host", &error);
  if (error) {
    fprintf(stderr, "Config missing stream.host: %s\n", error->message);
    g_error_free(error);
    goto done;
  }
  g_ptr_array_add(owned_strings, host);
  cfg->endpoint.host = host;

  error = NULL;
  cfg->endpoint.port = g_key_file_get_integer(kf, "stream", "port", &error);
  if (error) {
    fprintf(stderr, "Config missing/invalid stream.port: %s\n", error->message);
    g_error_free(error);
    goto done;
  }

  guint16 control_port = 8081;
  if (g_key_file_has_key(kf, "control", "port", NULL)) {
    error = NULL;
    gint configured_port = g_key_file_get_integer(kf, "control", "port", &error);
    if (error) {
      fprintf(stderr, "Invalid control.port: %s\n", error->message);
      g_error_free(error);
      goto done;
    }
    if (configured_port < 1 || configured_port > 65535) {
      fprintf(stderr, "control.port must be between 1 and 65535 (got %d)\n", configured_port);
      goto done;
    }
    control_port = (guint16)configured_port;
  }

  if (g_key_file_has_key(kf, "control", "combo_loop_mode", NULL)) {
    error = NULL;
    gchar *mode = g_key_file_get_string(kf, "control", "combo_loop_mode", &error);
    if (error) {
      fprintf(stderr, "Invalid control.combo_loop_mode: %s\n", error->message);
      g_error_free(error);
      goto done;
    }
    if (mode) {
      if (g_ascii_strcasecmp(mode, "entire") == 0 ||
          g_ascii_strcasecmp(mode, "full") == 0 ||
          g_ascii_strcasecmp(mode, "all") == 0) {
        combo_loop_full = TRUE;
      } else if (g_ascii_strcasecmp(mode, "final") == 0 ||
                 g_ascii_strcasecmp(mode, "last") == 0) {
        combo_loop_full = FALSE;
      } else {
        fprintf(stderr,
                "control.combo_loop_mode must be 'entire' or 'final' (got '%s')\n",
                mode);
        g_free(mode);
        goto done;
      }
    }
    g_free(mode);
  }

  GArray *seq_array = g_array_new(FALSE, FALSE, sizeof(SplashSeq));
  if (!seq_array) goto done;
  combo_defs = g_ptr_array_new_with_free_func((GDestroyNotify)pending_combo_free);
  if (!combo_defs) {
    g_array_free(seq_array, TRUE);
    goto done;
  }

  gsize n_groups = 0;
  gchar **groups = g_key_file_get_groups(kf, &n_groups);
  for (gsize i = 0; i < n_groups; ++i) {
    if (g_str_has_prefix(groups[i], SEQ_GROUP_PREFIX)) {
      gboolean has_order = g_key_file_has_key(kf, groups[i], "order", NULL);
      gboolean has_start = g_key_file_has_key(kf, groups[i], "start", NULL);
      gboolean has_end = g_key_file_has_key(kf, groups[i], "end", NULL);
      if (has_order) {
        if (has_start || has_end) {
          fprintf(stderr,
                  "Sequence group '%s' cannot mix order with start/end\n",
                  groups[i]);
          g_strfreev(groups);
          g_array_free(seq_array, TRUE);
          goto done;
        }
        PendingCombo *combo = NULL;
        if (!parse_combo_group(kf, groups[i], &combo, &error)) {
          fprintf(stderr, "Invalid combo sequence config: %s\n",
                  error ? error->message : "unknown error");
          if (error) g_error_free(error);
          error = NULL;
          g_strfreev(groups);
          g_array_free(seq_array, TRUE);
          g_ptr_array_free(combo_defs, TRUE);
          combo_defs = NULL;
          goto done;
        }
        g_ptr_array_add(combo_defs, combo);
      } else {
        if (!parse_sequence_group(kf, groups[i], owned_strings, seq_array, &error)) {
          fprintf(stderr, "Invalid sequence config: %s\n",
                  error ? error->message : "unknown error");
          if (error) g_error_free(error);
          error = NULL;
          g_strfreev(groups);
          g_array_free(seq_array, TRUE);
          g_ptr_array_free(combo_defs, TRUE);
          combo_defs = NULL;
          goto done;
        }
      }
    }
  }
  g_strfreev(groups);

  if (seq_array->len == 0) {
    fprintf(stderr, "Config must define at least one [sequence NAME] group\n");
    g_array_free(seq_array, TRUE);
    g_ptr_array_free(combo_defs, TRUE);
    combo_defs = NULL;
    goto done;
  }

  guint seq_count = seq_array->len;
  SplashSeq *seqs = g_new0(SplashSeq, seq_count);
  if (!seqs) {
    g_array_free(seq_array, TRUE);
    goto done;
  }

  for (guint i = 0; i < seq_count; ++i) {
    seqs[i] = g_array_index(seq_array, SplashSeq, i);
  }
  g_array_free(seq_array, TRUE);

  combo_count = combo_defs->len;
  if (combo_count > 0) {
    combo_array = g_new0(ComboSeq, combo_count);
    if (!combo_array) {
      g_ptr_array_free(combo_defs, TRUE);
      combo_defs = NULL;
      g_free(seqs);
      goto done;
    }
    for (guint i = 0; i < combo_count; ++i) {
      PendingCombo *pc = g_ptr_array_index(combo_defs, i);
      combo_array[i].count = (int)pc->parts->len;
      combo_array[i].indices = g_new0(int, combo_array[i].count);
      if (!combo_array[i].indices) {
        g_ptr_array_free(combo_defs, TRUE);
        combo_defs = NULL;
        for (guint k = 0; k <= i; ++k) {
          if (combo_array[k].indices) g_free(combo_array[k].indices);
        }
        g_free(combo_array);
        g_free(seqs);
        goto done;
      }
      combo_array[i].name = pc->name;
      g_ptr_array_add(owned_strings, pc->name);
      pc->name = NULL;
      combo_array[i].loop_at_end = pc->loop_at_end;
      for (guint j = 0; j < pc->parts->len; ++j) {
        const char *part_name = g_ptr_array_index(pc->parts, j);
        int found = -1;
        for (guint sidx = 0; sidx < seq_count; ++sidx) {
          if (g_strcmp0(seqs[sidx].name, part_name) == 0) {
            found = (int)sidx;
            break;
          }
        }
        if (found < 0) {
          fprintf(stderr,
                  "Combo sequence '%s' references unknown sequence '%s'\n",
                  combo_array[i].name ? combo_array[i].name : "?",
                  part_name);
          g_ptr_array_free(combo_defs, TRUE);
          combo_defs = NULL;
          for (guint k = 0; k <= i; ++k) {
            g_free(combo_array[k].indices);
          }
          g_free(combo_array);
          g_free(seqs);
          goto done;
        }
        combo_array[i].indices[j] = found;
      }
    }
  }
  if (combo_defs) {
    g_ptr_array_free(combo_defs, TRUE);
    combo_defs = NULL;
  }

  *seqs_out = seqs;
  *n_seqs_out = (int)seq_count;
  if (combos_out) *combos_out = combo_array;
  if (n_combos_out) *n_combos_out = (int)combo_count;
  *owned_strings_out = owned_strings;
  if (combo_loop_full_out) *combo_loop_full_out = combo_loop_full;
  if (http_port_out) *http_port_out = control_port;
  ok = TRUE;

done:
  if (!ok) {
    if (combo_array) {
      for (guint i = 0; i < combo_count; ++i) {
        g_free(combo_array[i].indices);
      }
      g_free(combo_array);
    }
  }
  if (combo_defs) {
    g_ptr_array_free(combo_defs, TRUE);
  }
  if (!ok) {
    g_ptr_array_free(owned_strings, TRUE);
  }
  g_free(config_abs);
  g_free(config_dir);
  g_key_file_free(kf);
  return ok;
}

int main(int argc, char **argv){
  gboolean cli_mode = FALSE;
  gboolean port_overridden = FALSE;
  guint16 http_port = 0;
  const char *config_path = NULL;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--cli")) {
      cli_mode = TRUE;
    } else if (g_str_has_prefix(argv[i], "--http-port=")) {
      const char *num = argv[i] + strlen("--http-port=");
      gchar *endptr = NULL;
      long port_val = strtol(num, &endptr, 10);
      if (!num[0] || (endptr && *endptr) || port_val < 1 || port_val > 65535) {
        fprintf(stderr, "Invalid --http-port value: %s\n", num);
        usage(argv[0]);
        return 2;
      }
      http_port = (guint16)port_val;
      port_overridden = TRUE;
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return 2;
    } else if (!config_path) {
      config_path = argv[i];
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  if (!config_path) { usage(argv[0]); return 2; }

  SplashSeq *seqs = NULL;
  int n_seqs = 0;
  ComboSeq *combos = NULL;
  int n_combos = 0;
  GPtrArray *owned_strings = NULL;
  SplashConfig cfg = {0};
  guint16 config_http_port = 8081;
  gboolean combo_loop_full = FALSE;
  if (!load_config(config_path, &cfg, &seqs, &n_seqs,
                   &combos, &n_combos,
                   &owned_strings, &combo_loop_full, &config_http_port)) {
    return 1;
  }

  if (!port_overridden) {
    http_port = config_http_port;
  }

  Splash *S = splash_new();
  AppCtx ctx = {0};
  ctx.splash = S;
  ctx.sequences = seqs;
  ctx.sequence_count = n_seqs;
  ctx.combos = combos;
  ctx.combo_count = n_combos;
  ctx.started = FALSE;
  ctx.combo_loop_full = combo_loop_full;
  ctx.loop = g_main_loop_new(NULL, FALSE);

  splash_set_event_cb(S, on_evt, &ctx);

  if (!splash_set_sequences(S, seqs, n_seqs)) {
    fprintf(stderr, "Failed to configure sequences\n");
    if (ctx.loop) g_main_loop_unref(ctx.loop);
    splash_free(S);
    g_free(seqs);
    free_combos(combos, n_combos);
    g_ptr_array_free(owned_strings, TRUE);
    return 1;
  }

  if (!splash_apply_config(S, &cfg)) {
    fprintf(stderr, "Failed to apply config\n");
    if (ctx.loop) g_main_loop_unref(ctx.loop);
    splash_free(S);
    g_free(seqs);
    free_combos(combos, n_combos);
    g_ptr_array_free(owned_strings, TRUE);
    return 1;
  }
  if (!splash_start(S)) {
    fprintf(stderr, "Failed to start\n");
    if (ctx.loop) g_main_loop_unref(ctx.loop);
    splash_free(S);
    g_free(seqs);
    free_combos(combos, n_combos);
    g_ptr_array_free(owned_strings, TRUE);
    return 1;
  }
  ctx.started = TRUE;

  GSocketService *http_service = g_socket_service_new();
  g_signal_connect(http_service, "incoming", G_CALLBACK(on_http_client), &ctx);
  gboolean http_ok = FALSE;
  GError *http_error = NULL;
  guint16 bind_port = http_port;
  if (bind_port == 0) {
    bind_port = config_http_port;
  }

  if (g_socket_listener_add_inet_port(G_SOCKET_LISTENER(http_service), bind_port,
                                      NULL, &http_error)) {
    http_ok = TRUE;
  } else {
    fprintf(stderr, "Failed to bind HTTP port %u: %s\n", bind_port,
            http_error ? http_error->message : "unknown error");
    if (http_error) g_error_free(http_error);
  }
  if (http_ok) {
    g_socket_service_start(http_service);
    fprintf(stderr,
            "HTTP control listening on http://127.0.0.1:%u/request/{start,stop,enqueue/<name>,list}\n",
            bind_port);
  } else {
    fprintf(stderr, "HTTP control disabled (no available port).\n");
    g_object_unref(http_service);
    http_service = NULL;
  }

  fprintf(stderr, "Configured sequences (%d):\n", n_seqs);
  for (int i = 0; i < n_seqs && i < 9; ++i) {
    fprintf(stderr, "  %d -> %s [%d..%d]\n", i + 1,
            seqs[i].name, seqs[i].start_frame, seqs[i].end_frame);
  }
  if (n_seqs > 9) {
    fprintf(stderr, "Additional sequences are available via API calls only.\n");
  }
  if (n_combos > 0) {
    fprintf(stderr, "Combo sequences (%d):\n", n_combos);
    for (int i = 0; i < n_combos; ++i) {
      fprintf(stderr, "  - %s [loop_at_end=%s] -> ",
              combos[i].name, combos[i].loop_at_end ? "true" : "false");
      for (int j = 0; j < combos[i].count; ++j) {
        int idx = combos[i].indices[j];
        const char *part = (idx >= 0 && idx < n_seqs) ? seqs[idx].name : "?";
        fprintf(stderr, "%s%s", j == 0 ? "" : ",", part);
      }
      fprintf(stderr, "\n");
    }
    fprintf(stderr, "Combo sequences can be enqueued via the HTTP API.\n");
  }
  if (cli_mode) {
    fprintf(stderr,
            "Interactive CLI enabled. Press 1-%d to enqueue; c=clear; s=start; x=stop; q=quit\n",
            n_seqs < 9 ? n_seqs : 9);
  }

  GIOChannel *stdin_chan = NULL;
  guint stdin_watch_id = 0;
  if (cli_mode) {
    if (!set_stdin_nonblock()) {
      fprintf(stderr, "Failed to configure stdin for non-blocking mode.\n");
    } else {
      stdin_chan = g_io_channel_unix_new(STDIN_FILENO);
      g_io_channel_set_encoding(stdin_chan, NULL, NULL);
      g_io_channel_set_buffered(stdin_chan, FALSE);
      stdin_watch_id = g_io_add_watch(stdin_chan, G_IO_IN, on_stdin_ready, &ctx);
    }
  }

  if (ctx.loop) {
    g_main_loop_run(ctx.loop);
  }

  if (stdin_watch_id) g_source_remove(stdin_watch_id);
  if (stdin_chan) g_io_channel_unref(stdin_chan);

  if (ctx.started) splash_stop(S);
  if (http_service) {
    g_socket_service_stop(http_service);
    g_object_unref(http_service);
  }
  if (ctx.loop) g_main_loop_unref(ctx.loop);
  splash_free(S);
  g_free(seqs);
  free_combos(combos, n_combos);
  g_ptr_array_free(owned_strings, TRUE);
  return 0;
}
