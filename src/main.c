#include "splashlib.h"
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>

static gboolean set_stdin_nonblock(void) {
  struct termios t; if (tcgetattr(STDIN_FILENO, &t)) return FALSE;
  t.c_lflag &= ~(ICANON | ECHO);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &t)) return FALSE;
  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  return fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void on_evt(SplashEventType type, int a, int b, const char *msg, void *user){
  (void)user;
  switch(type){
    case SPLASH_EVT_STARTED: fprintf(stderr, "[evt] started\n"); break;
    case SPLASH_EVT_STOPPED: fprintf(stderr, "[evt] stopped\n"); break;
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
    "  %s <config.ini>\n\n"
    "The configuration file must contain a [stream] group with keys:\n"
    "  input=/path/to/file.h265\n"
    "  fps=30.0\n"
    "  host=127.0.0.1\n"
    "  port=5600\n"
    "and one or more [sequence NAME] groups defining start/end frames.\n",
    p);
}

static gboolean parse_sequence_group(GKeyFile *kf, const gchar *group,
                                     GPtrArray *owned_strings,
                                     GArray *out_sequences,
                                     GError **error) {
  const gsize prefix_len = strlen(SEQ_GROUP_PREFIX);
  const gchar *raw = group + prefix_len;
  while (g_ascii_isspace(*raw)) raw++;

  if (*raw == '\0') {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "Sequence group '%s' is missing a name", group);
    return FALSE;
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
    return FALSE;
  }

  GError *local_error = NULL;
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

static gboolean load_config(const char *path,
                            SplashConfig *cfg,
                            SplashSeq **seqs_out,
                            int *n_seqs_out,
                            GPtrArray **owned_strings_out) {
  gboolean ok = FALSE;
  GError *error = NULL;
  GKeyFile *kf = g_key_file_new();
  if (!kf) return FALSE;

  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
    fprintf(stderr, "Failed to read config '%s': %s\n", path,
            error ? error->message : "unknown error");
    if (error) g_error_free(error);
    g_key_file_free(kf);
    return FALSE;
  }

  GPtrArray *owned_strings = g_ptr_array_new_with_free_func(g_free);
  if (!owned_strings) {
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
  g_ptr_array_add(owned_strings, input);
  cfg->input_path = input;

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

  GArray *seq_array = g_array_new(FALSE, FALSE, sizeof(SplashSeq));
  if (!seq_array) goto done;

  gsize n_groups = 0;
  gchar **groups = g_key_file_get_groups(kf, &n_groups);
  for (gsize i = 0; i < n_groups; ++i) {
    if (g_str_has_prefix(groups[i], SEQ_GROUP_PREFIX)) {
      if (!parse_sequence_group(kf, groups[i], owned_strings, seq_array, &error)) {
        fprintf(stderr, "Invalid sequence config: %s\n",
                error ? error->message : "unknown error");
        if (error) g_error_free(error);
        error = NULL;
        g_strfreev(groups);
        g_array_free(seq_array, TRUE);
        goto done;
      }
    }
  }
  g_strfreev(groups);

  if (seq_array->len == 0) {
    fprintf(stderr, "Config must define at least one [sequence NAME] group\n");
    g_array_free(seq_array, TRUE);
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

  *seqs_out = seqs;
  *n_seqs_out = (int)seq_count;
  *owned_strings_out = owned_strings;
  ok = TRUE;

done:
  if (!ok) {
    g_ptr_array_free(owned_strings, TRUE);
  }
  g_key_file_free(kf);
  return ok;
}

int main(int argc, char **argv){
  if (argc != 2) { usage(argv[0]); return 2; }

  const char *config_path = argv[1];

  SplashSeq *seqs = NULL;
  int n_seqs = 0;
  GPtrArray *owned_strings = NULL;
  SplashConfig cfg = {0};
  if (!load_config(config_path, &cfg, &seqs, &n_seqs, &owned_strings)) {
    return 1;
  }

  Splash *S = splash_new();
  splash_set_event_cb(S, on_evt, NULL);

  if (!splash_set_sequences(S, seqs, n_seqs)) {
    fprintf(stderr, "Failed to configure sequences\n");
    splash_free(S);
    g_free(seqs);
    g_ptr_array_free(owned_strings, TRUE);
    return 1;
  }

  if (!splash_apply_config(S, &cfg)) {
    fprintf(stderr, "Failed to apply config\n");
    splash_free(S);
    g_free(seqs);
    g_ptr_array_free(owned_strings, TRUE);
    return 1;
  }
  if (!splash_start(S)) {
    fprintf(stderr, "Failed to start\n");
    return 1;
  }

  fprintf(stderr, "Configured sequences (%d):\n", n_seqs);
  for (int i = 0; i < n_seqs && i < 9; ++i) {
    fprintf(stderr, "  %d -> %s [%d..%d]\n", i + 1,
            seqs[i].name, seqs[i].start_frame, seqs[i].end_frame);
  }
  if (n_seqs > 9) {
    fprintf(stderr, "Additional sequences are available via API calls only.\n");
  }
  fprintf(stderr, "Running. Press 1-%d to enqueue; c=clear; q=quit\n",
          n_seqs < 9 ? n_seqs : 9);
  set_stdin_nonblock();

  // Key loop
  for (;;) {
    char ch;
    if (read(STDIN_FILENO, &ch, 1)==1){
      if (ch=='q') { splash_quit(S); break; }
      else if (ch=='c') { splash_clear_next(S); }
      else if (ch>='1' && ch<='9') {
        int idx = ch - '1';
        if (idx < n_seqs) {
          splash_enqueue_next_by_index(S, idx);
        }
      }
    } else {
      // let GLib do the heavy lifting
      while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
      g_usleep(1000*5);
    }
  }

  splash_stop(S);
  splash_free(S);
  g_free(seqs);
  g_ptr_array_free(owned_strings, TRUE);
  return 0;
}
