# Agent Guidelines for splashscreen_rk

- Maintain compatibility with GStreamer 1.14 when touching RTSP code. Avoid
  using helpers that were introduced after 1.14 (for example,
  `gst_rtsp_media_is_prepared`). Prefer querying existing object properties or
  version guards when newer APIs are required.
- When updating `src/` sources, keep diagnostic logging in the existing
  `[rtsp]` format so deployments have consistent output.
- Build and test with `make` when possible. If GStreamer development packages
  are missing in the environment, record that limitation in the testing
  section.
