#include "splashlib.h"
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>

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

static void usage(const char *p){
  fprintf(stderr,
    "Usage:\n"
    "  %s <in.h265> <fps> --udp HOST PORT | --rtsp HOST PORT PATH\n\n"
    "Interactive:\n"
    "  1/2/3  -> enqueue named sequences demo: intro, loop, outro\n"
    "  c      -> clear pending queue\n"
    "  r      -> reconfigure (demo: toggles UDP<->RTSP)\n"
    "  q      -> quit\n", p);
}

int main(int argc, char **argv){
  if (argc < 5) { usage(argv[0]); return 2; }

  const char *in = argv[1];
  double fps = g_ascii_strtod(argv[2], NULL);
  SplashOutMode mode = SPLASH_OUT_UDP;
  const char *host = NULL, *path = "/splash";
  int port = 0;

  if (strcmp(argv[3],"--udp")==0 && argc>=6){
    mode = SPLASH_OUT_UDP; host = argv[4]; port = atoi(argv[5]);
  } else if (strcmp(argv[3],"--rtsp")==0 && argc>=7){
    mode = SPLASH_OUT_RTSP; host = argv[4]; port = atoi(argv[5]); path = argv[6];
  } else {
    usage(argv[0]); return 2;
  }

  Splash *S = splash_new();
  splash_set_event_cb(S, on_evt, NULL);

  // Demo: three named sequences; replace with your config loader later.
  // Make sure ranges start on IDRs for glitch-free switching.
  const SplashSeq SEQS[] = {
    { "intro",  0, 179 },
    { "loop", 300, 419 },
    { "outro", 600, 719 }
  };
  splash_set_sequences(S, SEQS, 3);

  SplashConfig cfg = {
    .input_path = in,
    .fps = fps,
    .out_mode = mode,
    .endpoint = { .host = host, .port = port, .path = path }
  };
  if (!splash_apply_config(S, &cfg)) {
    fprintf(stderr, "Failed to apply config\n");
    return 1;
  }
  if (!splash_start(S)) {
    fprintf(stderr, "Failed to start\n");
    return 1;
  }

  fprintf(stderr, "Running. Press 1/2/3 to enqueue intro/loop/outro; r=reconfigure; c=clear; q=quit\n");
  set_stdin_nonblock();

  // Key loop
  int rtsp_toggle = (mode==SPLASH_OUT_RTSP);
  for (;;) {
    char ch;
    if (read(STDIN_FILENO, &ch, 1)==1){
      if (ch=='q') { splash_quit(S); break; }
      else if (ch=='c') { splash_clear_next(S); }
      else if (ch=='1') { splash_enqueue_next_by_name(S, "intro"); }
      else if (ch=='2') { splash_enqueue_next_by_name(S, "loop"); }
      else if (ch=='3') { splash_enqueue_next_by_name(S, "outro"); }
      else if (ch=='r') {
        // Demo hot-reconfigure: toggle UDP <-> RTSP with same host/ports
        rtsp_toggle = !rtsp_toggle;
        SplashConfig nc = cfg;
        if (rtsp_toggle) {
          nc.out_mode = SPLASH_OUT_RTSP; nc.endpoint.path = "/splash";
          fprintf(stderr, "[demo] reconfig -> RTSP rtsp://%s:%d%s\n", nc.endpoint.host, nc.endpoint.port, nc.endpoint.path);
        } else {
          nc.out_mode = SPLASH_OUT_UDP;
          fprintf(stderr, "[demo] reconfig -> UDP %s:%d\n", nc.endpoint.host, nc.endpoint.port);
        }
        splash_apply_config(S, &nc);
        splash_start(S); // resumes reader + seek
      }
    } else {
      // let GLib do the heavy lifting
      while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
      g_usleep(1000*5);
    }
  }

  splash_stop(S);
  splash_free(S);
  return 0;
}
