#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <WS2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <winsock2.h>

#include <iostream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "IPHLPAPI.lib")
#pragma comment(lib, "Ws2_32.lib")

//////////////////////////////////////////////////////////////////////////
// global variables
static GMainContext* context_ = nullptr;
static GstElement* pipeline_ = nullptr;
static GMainLoop* main_loop_ = nullptr;
static GstRTSPServer* server_ = nullptr;
static gint server_id_; 
static GstState pipeline_state_ = GST_STATE_NULL;

constexpr const gchar* RTSP_SERVER_PORT = "9999";
constexpr const gchar* RTSP_SERVER_ADDR = "0.0.0.0";
constexpr const gchar* RTSP_1080_PATH = "/1";
constexpr const gchar* RTSP_720_PATH = "/2";

static std::unique_ptr<std::thread> gst_thread_; // gstreamer rtsp server thread
static std::vector<std::string> ip_addr_list_; // local ip address list
static std::string default_speaker_id_; // default speaker device id

// default settings
static int screen_index_ = 0;
static bool use_hardware_encoder_ = true;
static int target_bitrate_ = 4000;
static int target_fps_ = 30;

#if _DEBUG
static GstDebugLevel loglevel_ = GST_LEVEL_WARNING;
#else
static GstDebugLevel loglevel_ = GST_LEVEL_NONE;
#endif

//////////////////////////////////////////////////////////////////////////
// update gst pipeline state
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

//////////////////////////////////////////////////////////////////////////
// callback functions from gstreamer
static void client_disconnect_callback(GstRTSPClient* self,
                                       GstRTSPContext* ctx,
                                       gpointer user_data) {
  auto conn = gst_rtsp_client_get_connection(self);
  auto url = gst_rtsp_connection_get_url(conn);
  g_print(" [-]client disconnected, host:%s, port=%u\n", url->host, url->port);
}

static void client_connected_callback(GstRTSPServer* server,
                                      GstRTSPClient* object,
                                      gpointer user_data) {
  auto conn = gst_rtsp_client_get_connection(object);
  auto url = gst_rtsp_connection_get_url(conn);
  g_print(" [+]client connected, host:%s, port=%u\n", url->host, url->port);

  g_signal_connect(object, "teardown-request",
                   G_CALLBACK(client_disconnect_callback), nullptr);
}

static void media_constructed_callback(GstRTSPMediaFactory* factory,
                                       GstRTSPMedia* media,
                                       gpointer user_data) {
  // g_print("media constructed\n");
}

static void media_configure_callback(GstRTSPMediaFactory* factory,
                                     GstRTSPMedia* media,
                                     gpointer user_data) {
  // g_print("media configured\n");
}

//////////////////////////////////////////////////////////////////////////
// create gstreamer rtsp media factory
static GstRTSPMediaFactory* CreateRTSPMediaFactory(int width,
                                                   int height,
                                                   int bitrate,
                                                   bool audio) {
  GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
  gchar* pipeline = nullptr;
  gchar* monitor_index = nullptr;
  gchar* audio_pipeline = "";
  if (audio) {
    audio_pipeline = g_strdup_printf(
        "wasapi2src device=%s loopback=true ! queue "
        "! audioconvert ! queue ! "
        "avenc_aac "
        "bitrate=192000 ! "
        "rtpmp4apay name=pay1 pt=98",
        default_speaker_id_.c_str());
  }

  if (screen_index_ < 0) {
    monitor_index = g_strdup_printf("");
  } else {
    monitor_index = g_strdup_printf("monitor-index=%d", screen_index_);
  }

  if (use_hardware_encoder_) {
    pipeline = g_strdup_printf(
        "( d3d11screencapturesrc show-cursor=true %s ! queue ! "
        "d3d11convert ! video/x-raw(memory:D3D11Memory),width=%d,height=%d ! "
        "queue ! qsvh264enc "
        "bitrate=%d rate-control=cqp target-usage=7 ! rtph264pay "
        "name=pay0 pt=96 %s )",
        monitor_index, width, height, bitrate, audio_pipeline);

  } else {
    pipeline = g_strdup_printf(
        "( d3d11screencapturesrc show-cursor=true %s ! queue ! "
        "videoconvert ! "
        "openh264enc bitrate=%d rate-control=bitrate ! rtph264pay "
        "name=pay0 pt=96 %s )",
        monitor_index, bitrate, audio_pipeline);
  }

  gst_rtsp_media_factory_set_launch(factory, pipeline);

  g_free(monitor_index);
  g_free(pipeline);
  if (strlen(audio_pipeline) > 0) {
    g_free(audio_pipeline);
  }

  // set the protocol to use TCP
  gst_rtsp_media_factory_set_protocols(factory, GST_RTSP_LOWER_TRANS_UDP);
  gst_rtsp_media_factory_set_shared(factory, TRUE);

  g_signal_connect(factory, "media-constructed",
                   G_CALLBACK(media_constructed_callback), nullptr);
  g_signal_connect(factory, "media-configure",
                   G_CALLBACK(media_configure_callback), nullptr);

  return factory;
}

// create gstreamer rtsp server
static void InitGstPipeline() {
  context_ = g_main_context_new();
  g_main_context_push_thread_default(context_);

  auto session = gst_rtsp_session_pool_new();
  gst_rtsp_session_pool_set_max_sessions(session, 255);

  server_ = gst_rtsp_server_new();
  gst_rtsp_server_set_service(server_, RTSP_SERVER_PORT);

  GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);

  // add 1080p stream
  auto factory = CreateRTSPMediaFactory(1920, 1080, target_bitrate_, true);
  gst_rtsp_mount_points_add_factory(mounts, RTSP_1080_PATH, factory);

  // add 720p stream
  auto factory1 = CreateRTSPMediaFactory(1280, 720, target_bitrate_ / 2, true);
  gst_rtsp_mount_points_add_factory(mounts, RTSP_720_PATH, factory1);

  // bind the server to all network interfaces
  gst_rtsp_server_set_address(server_, RTSP_SERVER_ADDR);

  server_id_ = gst_rtsp_server_attach(server_, context_);
  if (server_id_ > 0) {
    g_print("");
    g_print(
        "\n======================= Play RTSP stream ready at: "
        "======================= \n");
    for (const auto& addr : ip_addr_list_) {
      int i = 1;
      while (i < 3) {
        g_print("rtsp://%s:%s/%d\n", addr.c_str(), RTSP_SERVER_PORT, i);
        i++;
      }
    }
    g_print(
        "===================================================================="
        "=");
    g_print("\n\n\n");

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
  g_object_unref(factory1);
  g_object_unref(server_);

  g_main_context_pop_thread_default(context_);
}

// stop gstreamer rtsp server
static void DeInitGstPipeline() {
  g_main_loop_quit(main_loop_);

  if (gst_thread_->joinable()) {
    gst_thread_->join();
  }
}

//////////////////////////////////////////////////////////////////////////
// enum display monitors
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor,
  HDC hdcMonitor,
  LPRECT lprcMonitor,
  LPARAM dwData) {
  int* count = (int*)dwData;
  int width = abs(lprcMonitor->right - lprcMonitor->left);
  int height = abs(lprcMonitor->bottom - lprcMonitor->top);
  g_print("Monitor: %d (%d,%d,%d,%d) [width=%d,height=%d]\n", *count,
    lprcMonitor->left, lprcMonitor->top, lprcMonitor->right,
    lprcMonitor->bottom, width, height);

  (*count)++;
  return TRUE;
}

// get current ip address
void GetCurrentIP() {
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cerr << "WSAStartup failed: " << WSAGetLastError() << std::endl;
    return;
  }

  PIP_ADAPTER_ADDRESSES pAddresses = NULL;
  ULONG outBufLen = 0;
  ULONG Iterations = 0;
  DWORD dwRetVal = 0;

  // Allocate a 15 KB buffer to start with.
  outBufLen = 15000;

  do {
    pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    if (pAddresses == NULL) {
      g_printerr("Memory allocation failed for IP_ADAPTER_ADDRESSES struct");
      WSACleanup();
      return;
    }

    dwRetVal = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL,
                                    pAddresses, &outBufLen);

    if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
      free(pAddresses);
      pAddresses = NULL;
    } else {
      break;
    }

    Iterations++;
  } while ((dwRetVal == ERROR_BUFFER_OVERFLOW) && (Iterations < 3));

  if (dwRetVal == NO_ERROR) {
    // If successful, output some information from the data we received
    PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses;
    while (pCurrAddresses) {
      bool type_filter = (pCurrAddresses->IfType == IF_TYPE_ETHERNET_CSMACD ||
                          pCurrAddresses->IfType == IF_TYPE_IEEE80211);
      if (type_filter && pCurrAddresses->OperStatus == IfOperStatusUp) {
        PIP_ADAPTER_UNICAST_ADDRESS pUnicast =
            pCurrAddresses->FirstUnicastAddress;
        while (pUnicast != NULL) {
          if (pUnicast->Address.lpSockaddr->sa_family ==
              AF_INET) {  // Check for IPv4
            char buffer[INET_ADDRSTRLEN] = {0};
            getnameinfo(pUnicast->Address.lpSockaddr,
                        pUnicast->Address.iSockaddrLength, buffer,
                        sizeof(buffer), NULL, 0, NI_NUMERICHOST);
            // g_print("Possible IP Address: %s\n", buffer);
            ip_addr_list_.push_back(buffer);
          }
          pUnicast = pUnicast->Next;
        }
      }
      pCurrAddresses = pCurrAddresses->Next;
    }
  } else {
    g_printerr("GetAdaptersAddresses failed with error: %u", dwRetVal);
  }

  if (pAddresses) {
    free(pAddresses);
  }

  WSACleanup();
}

// get default speaker device info
static std::string GetDefaultSpeakers() {
  GstDeviceMonitor* monitor;
  GstCaps* caps;

  monitor = gst_device_monitor_new();

  caps = gst_caps_new_empty_simple("audio/x-raw");
  gst_device_monitor_add_filter(monitor, "Audio/Source", caps);
  gst_caps_unref(caps);

  GList *devices, *device_l;
  devices = gst_device_monitor_get_devices(monitor);

  std::string result;

  for (device_l = devices; device_l != NULL; device_l = device_l->next) {
    GstDevice* device = GST_DEVICE(device_l->data);
    GstStructure* props = gst_device_get_properties(device);
    gboolean is_default = false;
    auto success =
        gst_structure_get_boolean(props, "device.default", &is_default);
    if (success) {
      gboolean is_loopback = false;
      gst_structure_get_boolean(props, "wasapi2.device.loopback", &is_loopback);
      if (is_default && is_loopback) {
        auto id = gst_structure_get_string(props, "device.id");
        g_print("Default speaker device: %s, id: %s\n",
                gst_device_get_display_name(device), id);

        result = id;
        break;
      }
    }

    gst_structure_free(props);
  }

  g_list_free_full(devices, g_object_unref);
  g_object_unref(monitor);

  return result;
}

// handle command line options
int HandleOptions(int argc, char** argv) {
  if (argc < 2) {
    g_print("Use default encoder settings!\n");
    return 0;
  }

  int ret = 1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-b") == 0) {
      if (i + 1 < argc) {
        target_bitrate_ = atoi(argv[i + 1]);
        i++;
        if (target_bitrate_ <= 0) {
          target_bitrate_ = 4000;
        }
      }
    } else if (strcmp(argv[i], "-e") == 0) {
      if (i + 1 < argc) {
        use_hardware_encoder_ = atoi(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-l") == 0) {
      if (i + 1 < argc) {
        loglevel_ = (GstDebugLevel)atoi(argv[i + 1]);
        i++;
      }
      if (loglevel_ < 0 || loglevel_ > 6) {
        loglevel_ = GST_LEVEL_ERROR;
      }
    }
  }

  return 1;
}

//////////////////////////////////////////////////////////////////////////
// entry point
int main(int argc, char** argv) {
  // get local ip address
  GetCurrentIP();
  // parse command line options
  HandleOptions(argc, argv);
  // set the default gst log level
  gst_debug_set_default_threshold(loglevel_);
  // init gstreamer
  gst_init(&argc, &argv);

  // get default speaker device name
  default_speaker_id_ = GetDefaultSpeakers();

  // get gstreamer version
  const gchar* version = gst_version_string();
  g_print("GStreamer version: %s\n\n\n", version);

  // get screen count
  int monitor_count = 0;
  if (!EnumDisplayMonitors(NULL, NULL, MonitorEnumProc,
                           (LPARAM)&monitor_count)) {
    g_printerr("EnumDisplayMonitors failed: %s\n", GetLastError());
    return -1;
  }
  g_print("Total of %d screens detected.\n", monitor_count);

  if (monitor_count > 1) {  // more than 1 screen
    // select screen to capture
    g_print("Please select the screen to capture: ");
    std::cin >> screen_index_;
    if (screen_index_ < 0 && screen_index_ > monitor_count) {
      g_printerr("Invalid screen index: %d\n", screen_index_);
      return -2;
    }
    std::cin.get();
  }

  g_print(
      "\nCapture screen %d\nEncoder settings: bitrate=%d, fps=%d, use "
      "hardware "
      "encoder=%d\n",
      screen_index_, target_bitrate_, target_fps_, use_hardware_encoder_);

  // create gst pipeline on a separate thread
  gst_thread_.reset(new std::thread(InitGstPipeline));

  std::cin.get();

  // stop gst pipeline
  DeInitGstPipeline();

  return 0;
}