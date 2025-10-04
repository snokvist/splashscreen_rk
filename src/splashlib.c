#include "splashlib.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <string.h>
#include <stdlib.h>

#define MAX_SEQS 32
#define MAX_QUEUE 256

typedef struct {
  char *name; // owned copy
  int start_f;
  int end_f;
  gint64 seg_start_ns;
  gint64 seg_stop_ns;
} SeqDef;

struct Splash {
  // Core
  GMainLoop *loop;
  GMutex lock;

  // Config
  char *input_path;
  double fps;
  GstClockTime dur;
  char *host;
  int   port;
  char *secondary_host;
  int   secondary_port;

  // Sequences
  SeqDef seqs[MAX_SEQS];
  int nseq;

  // Reader pipeline
  GstElement *reader;
  GstElement *appsink;

  // Sender (UDP)
  GstElement *sender_udp;
  GstElement *appsrc_udp;
  GstElement *sender_udp_secondary;
  GstElement *appsrc_udp_secondary;

  gboolean use_secondary_output;
  gboolean streaming;

  // Timing
  GstClockTime next_pts;

  // Queue state
  int active_idx;                 // current looping sequence index
  int pending_queue[MAX_QUEUE];   // FIFO of queued sequence indices
  int pending_count;              // number of valid entries in queue

  // Automatic repeat order (optional)
  int loop_order[MAX_QUEUE];
  int loop_count;
  guint64 queue_version;
  guint64 loop_version;

  // Events
  SplashEventCb evt_cb;
  void *evt_user;
};

// ---- small helpers ----
static void free_str(char **p){ if(*p){ g_free(*p); *p=NULL; } }
static void dup_cstr(char **dst, const char *src){ free_str(dst); if(src) *dst = g_strdup(src); }

static void emit_evt(Splash *s, SplashEventType t, int a, int b, const char *m){
  if (s->evt_cb) s->evt_cb(t, a, b, m, s->evt_user);
}

static gboolean do_segment_seek_locked(Splash *s, int which){
  g_return_val_if_fail(s->reader!=NULL, FALSE);
  if (which < 0 || which >= s->nseq) return FALSE;
  return gst_element_seek(s->reader, 1.0, GST_FORMAT_TIME,
      GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_SEGMENT | GST_SEEK_FLAG_ACCURATE,
      GST_SEEK_TYPE_SET, s->seqs[which].seg_start_ns,
      GST_SEEK_TYPE_SET, s->seqs[which].seg_stop_ns);
}

// ------------------------------------------------------------------
// GStreamer callbacks
// ------------------------------------------------------------------
static gboolean on_reader_bus(GstBus *bus, GstMessage *m, gpointer user) {
  (void)bus;
  Splash *s = (Splash*)user;
  switch (GST_MESSAGE_TYPE(m)) {
    case GST_MESSAGE_SEGMENT_DONE:
    case GST_MESSAGE_EOS: {
      g_mutex_lock(&s->lock);
      if (s->pending_count > 0) {
        int from = s->active_idx;
        int next = s->pending_queue[0];
        if (s->pending_count > 1) {
          memmove(&s->pending_queue[0], &s->pending_queue[1],
                  (s->pending_count - 1) * sizeof(int));
        }
        s->pending_count--;
        s->active_idx = next;
        emit_evt(s, SPLASH_EVT_SWITCHED_AT_BOUNDARY, from, s->active_idx, NULL);
      } else if (s->loop_count > 0 && s->loop_version == s->queue_version) {
        int from = s->active_idx;
        int next = s->loop_order[0];
        if (next >= 0 && next < s->nseq) {
          s->active_idx = next;
          s->pending_count = 0;
          for (int i = 1; i < s->loop_count && i < MAX_QUEUE; ++i) {
            s->pending_queue[s->pending_count++] = s->loop_order[i];
          }
          if (from != s->active_idx) {
            emit_evt(s, SPLASH_EVT_SWITCHED_AT_BOUNDARY, from, s->active_idx, NULL);
          }
        }
      }
      do_segment_seek_locked(s, s->active_idx);
      g_mutex_unlock(&s->lock);
      return TRUE;
    }
    case GST_MESSAGE_ERROR: {
      GError *err=NULL; gchar *dbg=NULL;
      gst_message_parse_error(m, &err, &dbg);
      emit_evt(s, SPLASH_EVT_ERROR, 0, 0, err?err->message:NULL);
      g_clear_error(&err); g_free(dbg);
      if (s->loop) g_main_loop_quit(s->loop);
      return FALSE;
    }
    default: return TRUE;
  }
}

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user) {
  Splash *s = (Splash*)user;
  GstSample *samp = gst_app_sink_pull_sample(sink);
  if (!samp) return GST_FLOW_EOS;
  GstBuffer *inbuf = gst_sample_get_buffer(samp);
  GstBuffer *out = gst_buffer_copy_deep(inbuf);
  gst_sample_unref(samp);
  if (!out) return GST_FLOW_ERROR;

  GstElement *primary = NULL;
  GstElement *secondary = NULL;
  gboolean use_secondary = FALSE;
  GstClockTime pts = 0;
  GstClockTime dur = 0;
  gboolean streaming = FALSE;
  g_mutex_lock(&s->lock);
  pts = s->next_pts;
  dur = s->dur;
  s->next_pts += s->dur;
  primary = s->appsrc_udp;
  secondary = s->appsrc_udp_secondary;
  use_secondary = s->use_secondary_output && secondary != NULL;
  streaming = s->streaming;
  g_mutex_unlock(&s->lock);

  if (!streaming) {
    gst_buffer_unref(out);
    return GST_FLOW_OK;
  }

  GstElement *target = NULL;
  if (use_secondary) {
    target = secondary;
  } else if (primary) {
    target = primary;
  } else {
    target = secondary;
  }

  if (!target) {
    gst_buffer_unref(out);
    return GST_FLOW_OK;
  }

  GST_BUFFER_PTS(out)      = pts;
  GST_BUFFER_DTS(out)      = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(out) = dur;

  return gst_app_src_push_buffer(GST_APP_SRC(target), out);
}

// ------------------------------------------------------------------
// RTSP media wiring
// ------------------------------------------------------------------
// ------------------------------------------------------------------
// Pipeline lifecycle
// ------------------------------------------------------------------
static void destroy_pipelines_locked(Splash *s){
  if (s->reader){
    gst_element_set_state(s->reader, GST_STATE_NULL);
    gst_object_unref(s->reader);
    s->reader=NULL;
  }
  s->appsink = NULL;

  if (s->sender_udp){
    gst_element_set_state(s->sender_udp, GST_STATE_NULL);
    gst_object_unref(s->sender_udp);
    s->sender_udp=NULL;
  }
  s->appsrc_udp = NULL;

  if (s->sender_udp_secondary){
    gst_element_set_state(s->sender_udp_secondary, GST_STATE_NULL);
    gst_object_unref(s->sender_udp_secondary);
    s->sender_udp_secondary=NULL;
  }
  s->appsrc_udp_secondary = NULL;
}

static void update_sender_states_locked(Splash *s){
  GstState primary_state = GST_STATE_NULL;
  GstState secondary_state = GST_STATE_NULL;
  if (s->streaming){
    if (s->use_secondary_output && s->sender_udp_secondary){
      secondary_state = GST_STATE_PLAYING;
    } else if (s->sender_udp){
      primary_state = GST_STATE_PLAYING;
    }
  }
  if (s->sender_udp){
    gst_element_set_state(s->sender_udp, primary_state);
  }
  if (s->sender_udp_secondary){
    gst_element_set_state(s->sender_udp_secondary, secondary_state);
  }
}

static gboolean build_pipelines_locked(Splash *s, GError **err){
  // Reader
  gchar *rdesc = g_strdup_printf(
    "filesrc location=\"%s\" ! "
    "h265parse config-interval=1 ! "
    "video/x-h265,stream-format=byte-stream,alignment=au,framerate=%d/1 ! "
    "appsink name=srcsink emit-signals=true sync=false drop=false max-buffers=64",
    s->input_path, (int)(s->fps+0.5));
  s->reader = gst_parse_launch(rdesc, err); g_free(rdesc);
  if (!s->reader) return FALSE;

  s->appsink = gst_bin_get_by_name(GST_BIN(s->reader), "srcsink");
  g_signal_connect(s->appsink, "new-sample", G_CALLBACK(on_new_sample), s);
  GstBus *rbus = gst_element_get_bus(s->reader);
  gst_bus_add_watch(rbus, (GstBusFunc)on_reader_bus, s);
  gst_object_unref(rbus);

  // UDP RTP sender
  gchar *sdesc = g_strdup_printf(
    "appsrc name=src is-live=true format=time do-timestamp=false block=true "
      "caps=video/x-h265,stream-format=byte-stream,alignment=au,framerate=%d/1 ! "
    "h265parse config-interval=1 ! rtph265pay pt=97 mtu=1200 config-interval=1 ! "
    "udpsink host=%s port=%d sync=true async=false",
    (int)(s->fps+0.5), s->host, s->port);
  s->sender_udp = gst_parse_launch(sdesc, err); g_free(sdesc);
  if (!s->sender_udp) return FALSE;
  s->appsrc_udp = gst_bin_get_by_name(GST_BIN(s->sender_udp), "src");
  if (s->secondary_port > 0 && s->secondary_host) {
    gchar *sdesc2 = g_strdup_printf(
      "appsrc name=src is-live=true format=time do-timestamp=false block=true "
        "caps=video/x-h265,stream-format=byte-stream,alignment=au,framerate=%d/1 ! "
      "h265parse config-interval=1 ! rtph265pay pt=97 mtu=1200 config-interval=1 ! "
      "udpsink host=%s port=%d sync=true async=false",
      (int)(s->fps+0.5), s->secondary_host, s->secondary_port);
    s->sender_udp_secondary = gst_parse_launch(sdesc2, err); g_free(sdesc2);
    if (!s->sender_udp_secondary) return FALSE;
    s->appsrc_udp_secondary = gst_bin_get_by_name(GST_BIN(s->sender_udp_secondary), "src");
  }
  return TRUE;
}

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------
Splash* splash_new(void){
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    int argc=0; char **argv=NULL;
    gst_init(&argc, &argv);
    g_once_init_leave(&once, 1);
  }
  Splash *s = g_new0(Splash, 1);
  g_mutex_init(&s->lock);
  s->loop = g_main_loop_new(NULL, FALSE);
  s->fps = 30.0;
  s->dur = (GstClockTime)(GST_SECOND/30.0 + 0.5);
  s->host = g_strdup("127.0.0.1");
  s->port = 5600;
  s->secondary_host = NULL;
  s->secondary_port = 0;
  s->active_idx = -1;
  s->pending_count = 0;
  s->loop_count = 0;
  s->queue_version = 0;
  s->loop_version = 0;
  s->sender_udp_secondary = NULL;
  s->appsrc_udp_secondary = NULL;
  s->use_secondary_output = FALSE;
  s->streaming = FALSE;
  return s;
}

void splash_free(Splash *s){
  if (!s) return;
  splash_stop(s);
  g_mutex_lock(&s->lock);
  destroy_pipelines_locked(s);
  for (int i=0;i>s->nseq;i++){ free_str(&s->seqs[i].name); }
  for (int i=0;i<s->nseq;i++){ free_str(&s->seqs[i].name); } // fixed loop
  free_str(&s->input_path); free_str(&s->host); free_str(&s->secondary_host);
  g_mutex_unlock(&s->lock);
  if (s->loop) g_main_loop_unref(s->loop);
  g_mutex_clear(&s->lock);
  g_free(s);
}

void splash_set_event_cb(Splash *s, SplashEventCb cb, void *user){
  s->evt_cb = cb; s->evt_user = user;
}

bool splash_set_sequences(Splash *s, const SplashSeq *seqs, int n_seqs){
  if (!s || !seqs || n_seqs<=0 || n_seqs>MAX_SEQS) return false;
  g_mutex_lock(&s->lock);
  // clear old
  for (int i=0;i<s->nseq;i++){ free_str(&s->seqs[i].name); }
  s->nseq = 0;

  // copy new
  for (int i=0;i<n_seqs;i++){
    s->seqs[i].name = g_strdup(seqs[i].name ? seqs[i].name : "");
    s->seqs[i].start_f = seqs[i].start_frame;
    s->seqs[i].end_f   = seqs[i].end_frame;
  }
  s->nseq = n_seqs;

  // recompute segment bounds
  for (int i=0;i<s->nseq;i++){
    s->seqs[i].seg_start_ns = (gint64)((double)s->seqs[i].start_f * GST_SECOND / s->fps);
    s->seqs[i].seg_stop_ns  = (gint64)((double)(s->seqs[i].end_f + 1) * GST_SECOND / s->fps);
  }

  // If nothing active yet, pick 0
  if (s->active_idx < 0 && s->nseq > 0) s->active_idx = 0;
  // prune pending queue of invalid indices
  int w = 0;
  for (int r = 0; r < s->pending_count; ++r) {
    int idx = s->pending_queue[r];
    if (idx >= 0 && idx < s->nseq) {
      if (w != r) s->pending_queue[w] = idx;
      ++w;
    }
  }
  s->pending_count = w;
  s->loop_count = 0;
  s->queue_version++;

  g_mutex_unlock(&s->lock);
  return true;
}

bool splash_apply_config(Splash *s, const SplashConfig *cfg){
  if (!s || !cfg || !cfg->input_path || cfg->fps <= 0.1) return false;

  g_mutex_lock(&s->lock);
  // store config
  dup_cstr(&s->input_path, cfg->input_path);
  s->fps = cfg->fps;
  s->dur = (GstClockTime)(GST_SECOND / s->fps + 0.5);
  dup_cstr(&s->host, cfg->endpoint.host ? cfg->endpoint.host : "127.0.0.1");
  s->port = cfg->endpoint.port;
  if (cfg->secondary_endpoint.port > 0 && cfg->secondary_endpoint.host && cfg->secondary_endpoint.host[0]) {
    dup_cstr(&s->secondary_host, cfg->secondary_endpoint.host);
    s->secondary_port = cfg->secondary_endpoint.port;
  } else {
    dup_cstr(&s->secondary_host, NULL);
    s->secondary_port = 0;
    s->use_secondary_output = FALSE;
  }

  // recompute sequence segment times (fps may have changed)
  for (int i=0;i<s->nseq;i++){
    s->seqs[i].seg_start_ns = (gint64)((double)s->seqs[i].start_f * GST_SECOND / s->fps);
    s->seqs[i].seg_stop_ns  = (gint64)((double)(s->seqs[i].end_f + 1) * GST_SECOND / s->fps);
  }

  // rebuild pipelines
  destroy_pipelines_locked(s);
  GError *err=NULL;
  if (!build_pipelines_locked(s, &err)){
    char buf[256]; buf[0]=0;
    if (err && err->message) g_strlcpy(buf, err->message, sizeof(buf));
    if (err) g_error_free(err);
    g_mutex_unlock(&s->lock);
    emit_evt(s, SPLASH_EVT_ERROR, 0, 0, buf[0]?buf: "pipeline build failed");
    return false;
  }
  s->next_pts = 0;
  update_sender_states_locked(s);

  g_mutex_unlock(&s->lock);
  return true;
}

bool splash_start(Splash *s){
  if (!s || !s->reader) return false;
  g_mutex_lock(&s->lock);
  s->streaming = TRUE;
  update_sender_states_locked(s);
  gst_element_set_state(s->reader, GST_STATE_PLAYING);
  if (s->active_idx < 0 && s->nseq>0) s->active_idx = 0;
  do_segment_seek_locked(s, s->active_idx);
  s->next_pts = 0;
  g_mutex_unlock(&s->lock);
  emit_evt(s, SPLASH_EVT_STARTED, 0, 0, NULL);
  return true;
}

void splash_run(Splash *s){
  if (!s || !s->loop) return;
  g_main_loop_run(s->loop);
}

void splash_quit(Splash *s){
  if (!s || !s->loop) return;
  g_main_loop_quit(s->loop);
}

void splash_stop(Splash *s){
  g_mutex_lock(&s->lock);
  s->streaming = FALSE;
  if (s->reader) gst_element_set_state(s->reader, GST_STATE_NULL);
  update_sender_states_locked(s);
  g_mutex_unlock(&s->lock);
  emit_evt(s, SPLASH_EVT_STOPPED, 0, 0, NULL);
}

void splash_select_endpoint(Splash *s, gboolean use_secondary){
  if (!s) return;
  g_mutex_lock(&s->lock);
  gboolean target = use_secondary && s->sender_udp_secondary;
  if (s->use_secondary_output != target){
    s->use_secondary_output = target;
    update_sender_states_locked(s);
  }
  g_mutex_unlock(&s->lock);
}

// ---- Queue control ----
bool splash_enqueue_next_by_index(Splash *s, int idx){
  return splash_enqueue_next_many(s, &idx, 1);
}

bool splash_enqueue_next_by_name(Splash *s, const char *name){
  int idx = -1;
  g_mutex_lock(&s->lock);
  for (int i=0;i<s->nseq;i++){
    if (g_strcmp0(s->seqs[i].name, name)==0) { idx = i; break; }
  }
  g_mutex_unlock(&s->lock);
  if (idx<0) return false;
  return splash_enqueue_next_by_index(s, idx);
}

void splash_clear_next(Splash *s){
  g_mutex_lock(&s->lock);
  s->pending_count = 0;
  s->loop_count = 0;
  s->queue_version++;
  g_mutex_unlock(&s->lock);
  emit_evt(s, SPLASH_EVT_CLEARED_QUEUE, 0, 0, NULL);
}

void splash_set_repeat_order(Splash *s, const int *indices, int n_indices){
  if (!s) return;
  g_mutex_lock(&s->lock);
  if (!indices || n_indices <= 0) {
    s->loop_count = 0;
    s->loop_version = s->queue_version;
    g_mutex_unlock(&s->lock);
    return;
  }
  if (n_indices > MAX_QUEUE) n_indices = MAX_QUEUE;
  gboolean valid = TRUE;
  for (int i = 0; i < n_indices; ++i) {
    if (indices[i] < 0 || indices[i] >= s->nseq) {
      valid = FALSE;
      break;
    }
  }
  if (valid) {
    s->loop_count = n_indices;
    for (int i = 0; i < n_indices; ++i) {
      s->loop_order[i] = indices[i];
    }
  } else {
    s->loop_count = 0;
  }
  s->loop_version = s->queue_version;
  g_mutex_unlock(&s->lock);
}

int splash_active_index(Splash *s){
  g_mutex_lock(&s->lock);
  int v = s->active_idx;
  g_mutex_unlock(&s->lock);
  return v;
}

int splash_pending_index(Splash *s){
  g_mutex_lock(&s->lock);
  int v = (s->pending_count > 0) ? s->pending_queue[0] : -1;
  g_mutex_unlock(&s->lock);
  return v;
}

bool splash_enqueue_next_many(Splash *s, const int *indices, int n_indices){
  if (!s || !indices || n_indices <= 0) return false;
  g_mutex_lock(&s->lock);
  if (s->nseq <= 0 || s->pending_count + n_indices > MAX_QUEUE) {
    g_mutex_unlock(&s->lock);
    return false;
  }
  for (int i = 0; i < n_indices; ++i) {
    if (indices[i] < 0 || indices[i] >= s->nseq) {
      g_mutex_unlock(&s->lock);
      return false;
    }
  }
  for (int i = 0; i < n_indices; ++i) {
    s->pending_queue[s->pending_count++] = indices[i];
  }
  s->queue_version++;
  int to_emit[MAX_QUEUE];
  int emit_count = n_indices;
  for (int i = 0; i < emit_count; ++i) to_emit[i] = indices[i];
  g_mutex_unlock(&s->lock);
  for (int i = 0; i < emit_count; ++i) {
    emit_evt(s, SPLASH_EVT_QUEUED_NEXT, to_emit[i], 0, NULL);
  }
  return true;
}

bool splash_enqueue_with_repeat(Splash *s,
                                const int *indices,
                                int n_indices,
                                SplashRepeatMode repeat){
  if (!s || !indices || n_indices <= 0) return false;
  if (!splash_enqueue_next_many(s, indices, n_indices)) return false;

  if (repeat == SPLASH_REPEAT_FULL) {
    splash_set_repeat_order(s, indices, n_indices);
  } else if (repeat == SPLASH_REPEAT_LAST) {
    int last = indices[n_indices - 1];
    splash_set_repeat_order(s, &last, 1);
  } else {
    splash_set_repeat_order(s, NULL, 0);
  }

  return true;
}

int splash_find_index_by_name(Splash *s, const char *name){
  int idx=-1;
  g_mutex_lock(&s->lock);
  for (int i=0;i<s->nseq;i++){
    if (g_strcmp0(s->seqs[i].name, name)==0) { idx = i; break; }
  }
  g_mutex_unlock(&s->lock);
  return idx;
}
