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

static GMainContext* context_ = nullptr;
static GstElement* pipeline_ = nullptr;
static GMainLoop* main_loop_ = nullptr;
static GstRTSPServer* server_ = nullptr;
static gint server_id_;
static GstState pipeline_state_ = GST_STATE_NULL;

constexpr const gchar* RTSP_PORT = "9999";
constexpr const gchar* RTSP_PATH = "/1";
constexpr const gchar* RTSP_ADDR = "0.0.0.0";

static std::unique_ptr<std::thread> gst_thread_;
static std::vector<std::string> ip_addr_list_;

static int screen_index_ = 0;
static bool use_hardware_encoder_ = true;
static int target_bitrate_ = 4000;
static int target_fps_ = 30;

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
  g_print("media constructed\n");
}

static void media_configure_callback(GstRTSPMediaFactory* factory,
                                     GstRTSPMedia* media,
                                     gpointer user_data) {
  g_print("media configured\n");
}

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

static void InitGstPipeline() {
  context_ = g_main_context_new();
  g_main_context_push_thread_default(context_);

  server_ = gst_rtsp_server_new();
  gst_rtsp_server_set_service(server_, RTSP_PORT);

  GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);
  GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();

  gchar* pipeline = nullptr;
  if (use_hardware_encoder_) {
    pipeline = g_strdup_printf(
        "( d3d11screencapturesrc show-cursor=true monitor-index=%d ! "
        "d3d11convert ! qsvh264enc "
        "bitrate=%d rate-control=cqp target-usage=7 gop-size=%d ! rtph264pay "
        "name=pay0 "
        "pt=96 )",
        screen_index_, target_bitrate_, target_fps_);
  } else {
    pipeline = g_strdup_printf(
        "( d3d11screencapturesrc show-cursor=true monitor-index=%d ! "
        "videoconvert ! x264enc "
        "bitrate=%d speed-preset=3 ! rtph264pay "
        "name=pay0 "
        "pt=96 )",
        screen_index_, target_bitrate_);
  }

  gst_rtsp_media_factory_set_launch(factory, pipeline);

  g_free(pipeline);

  // set the protocol to use TCP
  gst_rtsp_media_factory_set_protocols(factory, GST_RTSP_LOWER_TRANS_TCP);
  gst_rtsp_media_factory_set_shared(factory, TRUE);
  gst_rtsp_mount_points_add_factory(mounts, RTSP_PATH, factory);
  // bind the server to all network interfaces
  gst_rtsp_server_set_address(server_, RTSP_ADDR);

  server_id_ = gst_rtsp_server_attach(server_, context_);
  if (server_id_ > 0) {
    g_print("");
    g_print(
        "\n======================= Play RTSP stream ready at: "
        "======================= \n",
        RTSP_ADDR, RTSP_PORT, RTSP_PATH);
    for (const auto& addr : ip_addr_list_) {
      g_print("rtsp://%s:%s%s\n", addr.c_str(), RTSP_PORT, RTSP_PATH);
    }
    g_print("=====================================================================");
    g_print("\n\n\n");

    g_signal_connect(server_, "client-connected",
                     G_CALLBACK(client_connected_callback), nullptr);
    g_signal_connect(factory, "media-constructed",
                     G_CALLBACK(media_constructed_callback), nullptr);
    g_signal_connect(factory, "media-configure",
                     G_CALLBACK(media_configure_callback), nullptr);

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

int HandleOptions(int argc, char** argv) {
  if (argc < 2) {
    g_print("Use default settings!\n");
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
    }
  }

  return 1;
}

int main(int argc, char** argv) {
  GetCurrentIP();
  HandleOptions(argc, argv);

  gst_init(&argc, &argv);

  const gchar* version = gst_version_string();
  g_print("GStreamer version: %s\n\n\n", version);

  int monitor_count = 0;
  if (!EnumDisplayMonitors(NULL, NULL, MonitorEnumProc,
                           (LPARAM)&monitor_count)) {
    g_printerr("EnumDisplayMonitors failed: %s\n", GetLastError());
    return -1;
  }
  g_print("Total of %d screens detected.\n", monitor_count);

  // select screen to capture
  g_print("Please select the screen to capture: ");
  std::cin >> screen_index_;
  if (screen_index_ < 0 && screen_index_ > monitor_count) {
    g_printerr("Invalid screen index: %d\n", screen_index_);
    return -2;
  }

  g_print(
      "\nCapture screen %d\nEncoder settings: bitrate=%d, fps=%d, use hardware "
      "encoder=%d\n",
      screen_index_, target_bitrate_, target_fps_, use_hardware_encoder_);

  std::cin.get();

  // create gst pipeline on a separate thread
  gst_thread_.reset(new std::thread(InitGstPipeline));

  std::cin.get();

  // stop gst pipeline
  DeInitGstPipeline();

  return 0;
}