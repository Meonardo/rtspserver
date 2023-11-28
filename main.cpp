#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <iostream>
#include <string>
#include <thread>

static GMainContext* context_ = nullptr;
static GstElement* pipeline_ = nullptr;
static GMainLoop* main_loop_ = nullptr;
static GstRTSPServer* server_ = nullptr;
static gint server_id_;
static GstState pipeline_state_ = GST_STATE_NULL;

constexpr const gchar* RTSP_PORT = "8554";
constexpr const gchar* RTSP_PATH = "/test";
constexpr const gchar* RTSP_ADDR = "0.0.0.0";

std::unique_ptr<std::thread> gst_thread_;

static bool UpdatePipelineState(GstState new_state) {
  if (pipeline_state_ == new_state) {
    g_print(
        "the original state is the same as the new state, no need to change");
    return false;
  }
  if (pipeline_ == nullptr) {
    g_printerr("pipeline is null, please check again");
    return false;
  }

  // set the state of the pipeline to the desired state
  if (gst_element_set_state(pipeline_, new_state) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("unable to set the pipeline to the state: %d, old state: %d",
               new_state, pipeline_state_);
    return false;
  }

  // remember the current state
  pipeline_state_ = new_state;

  return true;
}

static void client_connected_callback(GstRTSPServer* self,
                                      GstRTSPClient* object,
                                      gpointer user_data) {
  auto conn = gst_rtsp_client_get_connection(object);
  auto ip = gst_rtsp_connection_get_ip(conn);
  auto url = gst_rtsp_connection_get_url(conn);
  g_print("client: %s connected, host:%s, port=%u\n", ip, url->host, url->port);
}

static void InitGstPipeline() {
  context_ = g_main_context_new();
  g_main_context_push_thread_default(context_);

  server_ = gst_rtsp_server_new();
  gst_rtsp_server_set_service(server_, RTSP_PORT);

  GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);
  GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();

  // set the launch string with the desired protocol (TCP or UDP)
  gst_rtsp_media_factory_set_launch(
      factory, "( videotestsrc ! x264enc ! rtph264pay name=pay0 pt=96 )");

  // set the protocol to use TCP
  gst_rtsp_media_factory_set_protocols(factory, GST_RTSP_LOWER_TRANS_TCP);

  gst_rtsp_media_factory_set_shared(factory, TRUE);
  gst_rtsp_mount_points_add_factory(mounts, RTSP_PATH, factory);

  // bind the server to all network interfaces
  gst_rtsp_server_set_address(server_, RTSP_ADDR);

  server_id_ = gst_rtsp_server_attach(server_, context_);
  if (server_id_ > 0) {
    g_print("Stream ready at rtsp://%s:%s%s\n", RTSP_ADDR, RTSP_PORT,
            RTSP_PATH);

    g_signal_connect(server_, "client-connected",
                     G_CALLBACK(client_connected_callback), nullptr);

    main_loop_ = g_main_loop_new(context_, FALSE);
    g_main_loop_run(main_loop_);

    g_main_loop_unref(main_loop_);
  } else {
    g_printerr("Can not start RTSP server");
  }

  g_object_unref(mounts);
  g_object_unref(factory);
  g_object_unref(server_);

  g_main_context_pop_thread_default(context_);
  g_main_context_unref(context_);
}

static void DeInitGstPipeline() {
  g_main_loop_quit(main_loop_);

  if (gst_thread_->joinable()) {
    gst_thread_->join();
  }
}

// test the server by using:
// gst-launch-1.0.exe playbin3 uri=rtsp://localhost:8554/test

int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  const gchar* version = gst_version_string();
  g_print("GStreamer version: %s\n", version);

  // create gst pipeline on a separate thread
  gst_thread_.reset(new std::thread(InitGstPipeline));

  std::cin.get();

  // stop gst pipeline
  DeInitGstPipeline();

  return 0;
}