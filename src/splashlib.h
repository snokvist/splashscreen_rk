#ifndef SPLASHLIB_H
#define SPLASHLIB_H

#include <gst/gst.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle
typedef struct Splash Splash;

// Named sequence: inclusive frame indices [start..end]
typedef struct {
  const char *name;   // non-owning utf8 string
  int start_frame;    // e.g., 0
  int end_frame;      // e.g., 180
} SplashSeq;

// UDP endpoint
typedef struct {
  const char *host;  // e.g., "127.0.0.1"
  int port;          // e.g., 5600
} SplashEndpoint;

// Configuration
typedef struct {
  const char *input_path;   // Annex-B H.265 elementary stream (AUD+VUI recommended)
  double fps;               // e.g., 30.0
  SplashEndpoint endpoint;  // UDP host+port
  SplashEndpoint secondary_endpoint;  // optional secondary UDP host+port (port<=0 disables)
} SplashConfig;

// Event callback (optional)
typedef enum {
  SPLASH_EVT_STARTED,
  SPLASH_EVT_STOPPED,
  SPLASH_EVT_SWITCHED_AT_BOUNDARY,  // payload: from_idx -> to_idx
  SPLASH_EVT_QUEUED_NEXT,           // payload: to_idx
  SPLASH_EVT_CLEARED_QUEUE,
  SPLASH_EVT_ERROR                  // payload: const char* message
} SplashEventType;

typedef void (*SplashEventCb)(SplashEventType type, int a, int b, const char *msg, void *user);

// ---- Lifecycle ----
Splash* splash_new(void);
void    splash_free(Splash *s);

// Configure named sequences (can be called any time; thread-safe)
bool splash_set_sequences(Splash *s, const SplashSeq *seqs, int n_seqs);

// Full (re)configuration of pipelines (safe to call while running)
bool splash_apply_config(Splash *s, const SplashConfig *cfg);

// Start/Run/Stop
bool splash_start(Splash *s);
void splash_run(Splash *s);    // blocks: runs internal GMainLoop
void splash_quit(Splash *s);   // quits the loop (non-blocking)
void splash_stop(Splash *s);   // stops pipelines

// ---- Control / Queue API ----
// Multi-queue model: current loops forever; queued entries take over at segment boundaries.
// Returns false if any index/name is invalid or the queue would overflow.
bool splash_enqueue_next_by_index(Splash *s, int idx);
bool splash_enqueue_next_by_name(Splash *s, const char *name);
bool splash_enqueue_next_many(Splash *s, const int *indices, int n_indices);

// Convenience helper for the common "enqueue + choose repeat" workflow. The
// repeat behavior controls what happens after the queued items finish:
//   SPLASH_REPEAT_NONE  -> disable any custom repeat order.
//   SPLASH_REPEAT_LAST  -> loop the last queued index indefinitely.
//   SPLASH_REPEAT_FULL  -> loop the full queued order.
// Returns false if queuing fails (invalid indices or queue full).
typedef enum {
  SPLASH_REPEAT_NONE = 0,
  SPLASH_REPEAT_LAST,
  SPLASH_REPEAT_FULL,
} SplashRepeatMode;

bool splash_enqueue_with_repeat(Splash *s,
                                const int *indices,
                                int n_indices,
                                SplashRepeatMode repeat);

void splash_clear_next(Splash *s);

// Configure automatic looping order once the queue drains. Passing NULL or
// n_indices<=0 disables any custom repeat behavior.
void splash_set_repeat_order(Splash *s, const int *indices, int n_indices);

// Query helpers (optional)
int  splash_active_index(Splash *s);          // -1 if none
int  splash_pending_index(Splash *s);         // -1 if none
int  splash_find_index_by_name(Splash *s, const char *name);

// Logging / events
void splash_set_event_cb(Splash *s, SplashEventCb cb, void *user);

// Runtime control helpers
void splash_select_endpoint(Splash *s, gboolean use_secondary);

#ifdef __cplusplus
}
#endif
#endif
