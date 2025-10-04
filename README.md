# Splashscreen RK

Splashscreen RK is a GStreamer-based utility that loops H.265 frame ranges from
an input elementary stream and forwards them over RTP. It supports simple HTTP
controls and optional CLI shortcuts for enqueueing individual sequences or
predefined combo playlists.

## Requirements

Install the development toolchain and GStreamer components before building:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential pkg-config \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgstrtspserver-1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly
```

## Building

Once the dependencies are present, build the project with `make`. The default
rule compiles the CLI and HTTP control application as well as the supporting
`splashlib` GStreamer wrapper.

## Configuration

Configuration files use INI syntax. The sample [`config/demo.ini`](config/demo.ini)
illustrates every available section:

- `[stream]`
  - `input`: Path to an H.265 elementary stream file that contains repeated key
    frames.
  - `fps`: Frame rate of the input material (double).
  - `host`: Destination IP for the RTP/UDP output.
  - `port`: Destination UDP port.
- `[control]`
  - `port`: HTTP control port (defaults to `8081` if omitted).
  - `combo_loop_mode`: Controls how combo playlists repeat once the queue drains.
    Use `final` to keep looping only the combo's last sequence, or `entire` to
    replay the whole combo again. Only combos with `loop_at_end=true` will
    auto-repeat.
- `[sequence NAME]` sections define either raw sequences or combo playlists:
  - For raw sequences, provide `start` and `end` frame numbers.
  - For combos, provide `order` with comma-separated sequence names. Optionally
    add `loop_at_end=true` to mark the combo as eligible for looping when
    `combo_loop_mode` is `entire`.

## Running

Start the program by pointing it at your INI file:

```sh
./splashscreen_rk --config config/demo.ini
```

When CLI mode is enabled (`--cli`), press `1-9` to enqueue individual sequences,
`c` to clear the queue, `s` to start, `x` to stop, and `q` to quit. All control
features are also available over HTTP via `GET /request/{start,stop,list}` and
`GET /request/enqueue/<name>` for sequences or combos.

## Preparing H.265 Inputs

To create an all-I-frame (keyframe) H.265 file from a PNG sequence, use:

```sh
gst-launch-1.0 -e \
  multifilesrc location="OpenIPC_intro_v2_%05d.png" start-index=0 stop-index=180 \
    caps="image/png,framerate=(fraction)30/1" ! \
  pngdec ! videoconvert ! video/x-raw,format=I420 ! \
  x265enc speed-preset=ultrafast tune=zerolatency \
          option-string="keyint=1:scenecut=0:bframes=0:rc-lookahead=0:open-gop=0:aud=1:repeat-headers=1" ! \
  h265parse config-interval=1 ! \
  video/x-h265,stream-format=byte-stream,alignment=au ! \
  filesink location=spinner_ai_1080p30.h265
```

This pipeline encodes each PNG as an independent frame, producing a stream that
works well with the sequence-based segment seeking approach used by
Splashscreen RK.

## HTTP API Summary

- `GET /request/start` — start playback.
- `GET /request/stop` — stop playback.
- `GET /request/list` — enumerate sequences and combos with their orders.
- `GET /request/enqueue/<name>` — enqueue either a single sequence or a combo by
  name. When combos marked with `loop_at_end=true` are enqueued, they will
  repeat according to `combo_loop_mode` until the queue is updated.

## License

Refer to the repository for licensing details.
