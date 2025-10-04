#include "splashlib.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <string.h>
#include <stdlib.h>

#define MAX_SEQS 32

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
  SplashOutMode out_mode;
  char *host;
  int   port;
  char *path; // RTSP only

  // Sequences
  SeqDef seqs[MAX_SEQS];
  int nseq;

  // Reader pipeline
  GstElement *reader;
  GstElement *appsink;

  // Sender (UDP)
  GstElement *sender_udp;
  GstElement *appsrc_udp;

  // Sender (RTSP)
  GstRTSPServer *rtsp_server;
  GstRTSPMediaFactory *rtsp_factory;
  GstElement *appsrc_rtsp;

  GstCaps *current_caps;

  // Timing
  GstClockTime next_pts;

  // Two-queue state
  int active_idx;   // current looping sequence index
  int pending_idx;  // -1 none

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
// RTSP debug helpers (no version-sensitive parsing)
// ------------------------------------------------------------------
static inline const gchar* rtsp_client_ip(GstRTSPClient *client) {
#if GST_CHECK_VERSION(1,14,0)
  GstRTSPConnection *conn = gst_rtsp_client_get_connection(client);
  if (conn) return gst_rtsp_connection_get_ip(conn);
#endif
  return "?";
}
static inline const gchar* rtsp_ctx_uri(GstRTSPContext *ctx) {
  if (ctx && ctx->uri && ctx->uri->abspath) return ctx->uri->abspath;
  return "?";
}

static gboolean log_rtsp_req(const char *method, GstRTSPClient *client, GstRTSPContext *ctx, gpointer user){
  (void)user;
  g_printerr("[rtsp] %s %s from %s\n", method, rtsp_ctx_uri(ctx), rtsp_client_ip(client));
  return FALSE; // continue normal handling
}

static gboolean on_req_options (GstRTSPClient *c, GstRTSPContext *ctx, gpointer u){ return log_rtsp_req("OPTIONS",  c, ctx, u); }
static gboolean on_req_describe(GstRTSPClient *c, GstRTSPContext *ctx, gpointer u){ return log_rtsp_req("DESCRIBE", c, ctx, u); }
static gboolean on_req_setup   (GstRTSPClient *c, GstRTSPContext *ctx, gpointer u){ return log_rtsp_req("SETUP",    c, ctx, u); }
static gboolean on_req_play    (GstRTSPClient *c, GstRTSPContext *ctx, gpointer u){ return log_rtsp_req("PLAY",     c, ctx, u); }
static gboolean on_req_teardown(GstRTSPClient *c, GstRTSPContext *ctx, gpointer u){ return log_rtsp_req("TEARDOWN", c, ctx, u); }

static void on_client_connected(GstRTSPServer *server, GstRTSPClient *client, gpointer user) {
  (void)server; (void)user;
  g_printerr("[rtsp] client-connected from %s\n", rtsp_client_ip(client));
  // Hook common RTSP request signals
  g_signal_connect(client, "options-request",  G_CALLBACK(on_req_options),  user);
  g_signal_connect(client, "describe-request", G_CALLBACK(on_req_describe), user);
  g_signal_connect(client, "setup-request",    G_CALLBACK(on_req_setup),    user);
  g_signal_connect(client, "play-request",     G_CALLBACK(on_req_play),     user);
  g_signal_connect(client, "teardown-request", G_CALLBACK(on_req_teardown), user);
}

static gboolean bind_media_appsrc(Splash *s, GstRTSPMedia *media, const char *tag) {
  GstElement *bin = gst_rtsp_media_get_element(media);
  if (!bin) {
    g_printerr("[rtsp] %s: missing bin\n", tag);
    return FALSE;
  }

  GstElement *appsrc = gst_bin_get_by_name(GST_BIN(bin), "src");
  gst_object_unref(bin);
  if (!appsrc) {
    g_printerr("[rtsp] %s: missing appsrc\n", tag);
    return FALSE;
  }

  g_object_set(appsrc, "is-live", TRUE, "format", GST_FORMAT_TIME,
               "block", TRUE, "do-timestamp", FALSE, NULL);

  g_mutex_lock(&s->lock);
  if (s->appsrc_rtsp != appsrc) {
    if (s->appsrc_rtsp)
      gst_object_unref(s->appsrc_rtsp);
    s->appsrc_rtsp = gst_object_ref(appsrc);
  }
  if (s->current_caps)
    gst_app_src_set_caps(GST_APP_SRC(appsrc), s->current_caps);
  g_mutex_unlock(&s->lock);

  g_printerr("[rtsp] %s: appsrc ready\n", tag);
  gst_object_unref(appsrc);
  return TRUE;
}

static void on_media_prepared(GstRTSPMedia *media, Splash *s) {
  bind_media_appsrc(s, media, "media-prepared");
}

static void on_media_unprepared(GstRTSPMedia *media, Splash *s) {
  (void)media;
  gboolean had_appsrc = FALSE;
  g_mutex_lock(&s->lock);
  if (s->appsrc_rtsp) {
    gst_object_unref(s->appsrc_rtsp);
    s->appsrc_rtsp = NULL;
    had_appsrc = TRUE;
  }
  g_mutex_unlock(&s->lock);
  if (had_appsrc)
    g_printerr("[rtsp] media-unprepared: appsrc removed\n");
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
      if (s->pending_idx >= 0) {
        int from = s->active_idx;
        s->active_idx = s->pending_idx;
        s->pending_idx = -1;
        emit_evt(s, SPLASH_EVT_SWITCHED_AT_BOUNDARY, from, s->active_idx, NULL);
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

  GstCaps *caps = gst_sample_get_caps(samp);
  GstBuffer *inbuf = gst_sample_get_buffer(samp);

  g_mutex_lock(&s->lock);

  if (caps) {
    if (!s->current_caps || !gst_caps_is_equal(caps, s->current_caps)) {
      if (s->current_caps) gst_caps_unref(s->current_caps);
      s->current_caps = gst_caps_copy(caps);
      if (s->appsrc_udp)
        gst_app_src_set_caps(GST_APP_SRC(s->appsrc_udp), s->current_caps);
      if (s->appsrc_rtsp)
        gst_app_src_set_caps(GST_APP_SRC(s->appsrc_rtsp), s->current_caps);
    }
  }

  GstClockTime pts = s->next_pts;
  GstClockTime dur = s->dur;
  s->next_pts += s->dur;

  GstElement *udp_target = NULL;
  GstElement *rtsp_target = NULL;
  if (s->out_mode == SPLASH_OUT_UDP) {
    if (s->appsrc_udp)
      udp_target = gst_object_ref(s->appsrc_udp);
  } else if (s->appsrc_rtsp) {
    rtsp_target = gst_object_ref(s->appsrc_rtsp);
  }

  g_mutex_unlock(&s->lock);

  if (!udp_target && !rtsp_target) {
    gst_sample_unref(samp);
    return GST_FLOW_OK;
  }

  GstFlowReturn fr = GST_FLOW_OK;

  if (udp_target) {
    GstBuffer *out = gst_buffer_copy_deep(inbuf);
    GST_BUFFER_PTS(out)      = pts;
    GST_BUFFER_DTS(out)      = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(out) = dur;
    fr = gst_app_src_push_buffer(GST_APP_SRC(udp_target), out);
    gst_object_unref(udp_target);
  }

  if (rtsp_target) {
    GstBuffer *out = gst_buffer_copy_deep(inbuf);
    GST_BUFFER_PTS(out)      = pts;
    GST_BUFFER_DTS(out)      = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(out) = dur;
    GstFlowReturn cur_fr = gst_app_src_push_buffer(GST_APP_SRC(rtsp_target), out);
    if (fr == GST_FLOW_OK && cur_fr != GST_FLOW_OK)
      fr = cur_fr;
    gst_object_unref(rtsp_target);
  }

  gst_sample_unref(samp);
  return fr;
}

// ------------------------------------------------------------------
// RTSP media wiring
// ------------------------------------------------------------------
static void on_media_configure(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user) {
  (void)factory;
  Splash *s = (Splash*)user;
  gst_rtsp_media_set_reusable(media, TRUE);

  bind_media_appsrc(s, media, "media-configure");
  g_signal_connect(media, "prepared", G_CALLBACK(on_media_prepared), s);
  g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), s);
}

// ------------------------------------------------------------------
// Pipeline lifecycle
// ------------------------------------------------------------------
static void destroy_pipelines_locked(Splash *s){
  if (s->reader){ gst_element_set_state(s->reader, GST_STATE_NULL); gst_object_unref(s->reader); s->reader=NULL; }
  s->appsink = NULL;

  if (s->sender_udp){ gst_element_set_state(s->sender_udp, GST_STATE_NULL); gst_object_unref(s->sender_udp); s->sender_udp=NULL; }
  s->appsrc_udp = NULL;

  if (s->appsrc_rtsp) { gst_object_unref(s->appsrc_rtsp); s->appsrc_rtsp = NULL; }
  if (s->rtsp_server){ g_object_unref(s->rtsp_server); s->rtsp_server=NULL; }
  s->rtsp_factory=NULL;
  if (s->current_caps){ gst_caps_unref(s->current_caps); s->current_caps=NULL; }
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

  if (s->out_mode == SPLASH_OUT_UDP) {
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
    if (s->current_caps)
      gst_app_src_set_caps(GST_APP_SRC(s->appsrc_udp), s->current_caps);
  } else {
    // RTSP server (allow UDP and TCP interleaved for compatibility)
    s->rtsp_server = gst_rtsp_server_new();
    gst_rtsp_server_set_address(s->rtsp_server, s->host);
    gchar *service = g_strdup_printf("%d", s->port);
    gst_rtsp_server_set_service(s->rtsp_server, service);
    g_free(service);

    s->rtsp_factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_protocols(
        s->rtsp_factory,
        GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP);
    gst_rtsp_media_factory_set_shared(s->rtsp_factory, TRUE);

    gchar *launch = g_strdup_printf(
      "( appsrc name=src is-live=true format=time block=true do-timestamp=false "
        "caps=video/x-h265,stream-format=byte-stream,alignment=au,framerate=%d/1 ! "
        "h265parse config-interval=1 ! rtph265pay pt=97 name=pay0 )", (int)(s->fps+0.5));
    gst_rtsp_media_factory_set_launch(s->rtsp_factory, launch);
    g_free(launch);
    g_signal_connect(s->rtsp_factory, "media-configure", G_CALLBACK(on_media_configure), s);

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(s->rtsp_server);
    gst_rtsp_mount_points_add_factory(mounts, s->path ? s->path : "/splash", s->rtsp_factory);
    g_object_unref(mounts);

    gst_rtsp_server_attach(s->rtsp_server, NULL);

    // Debug: listening + per-client and per-request logs
    g_signal_connect(s->rtsp_server, "client-connected", G_CALLBACK(on_client_connected), s);
    g_printerr("[rtsp] listening on rtsp://%s:%d%s (UDP+TCP)\n",
               s->host ? s->host : "0.0.0.0",
               s->port,
               s->path ? s->path : "/splash");
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
  s->out_mode = SPLASH_OUT_UDP;
  s->host = g_strdup("127.0.0.1");
  s->port = 5600;
  s->path = g_strdup("/splash");
  s->active_idx = -1;
  s->pending_idx = -1;
  return s;
}

void splash_free(Splash *s){
  if (!s) return;
  splash_stop(s);
  g_mutex_lock(&s->lock);
  destroy_pipelines_locked(s);
  for (int i=0;i>s->nseq;i++){ free_str(&s->seqs[i].name); }
  for (int i=0;i<s->nseq;i++){ free_str(&s->seqs[i].name); } // fixed loop
  free_str(&s->input_path); free_str(&s->host); free_str(&s->path);
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
  // clamp pending if needed
  if (s->pending_idx >= s->nseq) s->pending_idx = -1;

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
  s->out_mode = cfg->out_mode;
  dup_cstr(&s->host, cfg->endpoint.host ? cfg->endpoint.host : "127.0.0.1");
  s->port = cfg->endpoint.port;
  if (s->out_mode == SPLASH_OUT_RTSP) dup_cstr(&s->path, cfg->endpoint.path ? cfg->endpoint.path : "/splash");

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

  g_mutex_unlock(&s->lock);
  return true;
}

bool splash_start(Splash *s){
  if (!s || !s->reader) return false;
  g_mutex_lock(&s->lock);
  if (s->out_mode==SPLASH_OUT_UDP && s->sender_udp)
    gst_element_set_state(s->sender_udp, GST_STATE_PLAYING);

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
  if (s->reader) gst_element_set_state(s->reader, GST_STATE_NULL);
  if (s->sender_udp) gst_element_set_state(s->sender_udp, GST_STATE_NULL);
  g_mutex_unlock(&s->lock);
  emit_evt(s, SPLASH_EVT_STOPPED, 0, 0, NULL);
}

// ---- Queue control ----
bool splash_enqueue_next_by_index(Splash *s, int idx){
  g_mutex_lock(&s->lock);
  if (idx < 0 || idx >= s->nseq || idx == s->active_idx) {
    g_mutex_unlock(&s->lock);
    return false;
  }
  s->pending_idx = idx;
  int to = s->pending_idx;
  g_mutex_unlock(&s->lock);
  emit_evt(s, SPLASH_EVT_QUEUED_NEXT, to, 0, NULL);
  return true;
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
  s->pending_idx = -1;
  g_mutex_unlock(&s->lock);
  emit_evt(s, SPLASH_EVT_CLEARED_QUEUE, 0, 0, NULL);
}

int splash_active_index(Splash *s){
  g_mutex_lock(&s->lock);
  int v = s->active_idx;
  g_mutex_unlock(&s->lock);
  return v;
}

int splash_pending_index(Splash *s){
  g_mutex_lock(&s->lock);
  int v = s->pending_idx;
  g_mutex_unlock(&s->lock);
  return v;
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
