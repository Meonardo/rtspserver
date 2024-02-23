# rtspserver
a simple RTSP server created by using GStreamer.
- video source is from screen capture;
- audio source is from default audio output device;

### Notic
- Windows 64bit only;
- GStreamer `v1.22.7`, install both dev & runtime;
  
### Build
```bash
cmake -B .\build -DGSTREAMER_PKG_DIR="D:\gstreamer\1.0\msvc_x86_64\lib\pkgconfig"
```

### Usage
- Run the default command `rtspserver.exe` to start the server on port `554` with Intel QSV encoder(const bps=4000k) enabled;
- Full command:
  ```bash
  rtspserver.exe -p {port_number} -b {encode_bitrate} -l {log_level} -e {encode_method}
  ```

### Pipelines
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
