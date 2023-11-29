# rtspserver
a simple RTSP server created by using GStreamer.

### Notice:
- Windows 64bit only;
- GStreamer `v1.22.7`;
### Pipelines:
- Intel QSV encoder:
  ```bash
  gst-launch-1.0 -v d3d11screencapturesrc show-cursor=true ! d3d11convert ! qsvh264enc low-latency=true bitrate=4000 max-bitrate=8000 ! h264parse ! matroskamux ! filesink location="c:/Users/Meonardo/Downloads/screen_capture.mkv"
  ```
- X264 encoder
  ```bash
  gst-launch-1.0 -v d3d11screencapturesrc show-cursor=true ! videoconvert ! x264enc ! h264parse ! matroskamux ! filesink location="c:/Users/Meonardo/Downloads/screen_capture.mkv"
  ```
- Play
  ```bash
  gst-launch-1.0.exe rtspsrc location=rtsp://127.0.0.1:9999/1 buffer-mode=0 ! rtph264depay ! h264parse ! qsvh264dec ! fpsdisplaysink
  ```
