#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace {

constexpr const char* kVersion = "0.1.0";
constexpr const char* kDefaultHost = "0.0.0.0";
constexpr int kDefaultPort = 8080;

volatile std::sig_atomic_t g_stop_requested = 0;

void handle_signal(int) {
  g_stop_requested = 1;
}

void print_usage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << " [--host HOST] [--port PORT] [--web DIR]\n"
            << "  --host HOST   listen address, default " << kDefaultHost << "\n"
            << "  --port PORT   listen port,   default " << kDefaultPort << "\n"
            << "  --web DIR     static files root (front end), optional\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  std::string host = kDefaultHost;
  int port = kDefaultPort;
  std::string web_dir;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (arg == "--web" && i + 1 < argc) {
      web_dir = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown or incomplete option: " << arg << "\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  httplib::Server svr;
  svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.body.empty()) {
      nlohmann::json j = {{"error", "request failed"}};
      res.set_content(j.dump(), "application/json");
    }
  });

  svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("Oj System Server is running", "text/plain; charset=utf-8");
  });

  svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    nlohmann::json j = {{"status", "ok"},
                        {"service", "oj-system"},
                        {"version", kVersion}};
    res.set_content(j.dump(), "application/json");
  });

  if (!web_dir.empty()) {
    if (svr.set_mount_point("/", web_dir)) {
      std::cout << "[oj] static files mounted from " << web_dir << "\n";
    } else {
      std::cerr << "[oj] cannot mount static files from " << web_dir << "\n";
      return 1;
    }
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  bool listen_ok = false;
  std::thread server_thread([&] { listen_ok = svr.listen(host.c_str(), port); });

  std::cout << "[oj] starting server on " << host << ":" << port << " (version "
            << kVersion << ")\n";
  std::cout << "[oj] press Ctrl+C to stop\n";

  while (!g_stop_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "\n[oj] graceful shutdown requested, stopping server...\n";
  svr.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  if (listen_ok) {
    std::cout << "[oj] server stopped cleanly\n";
    return 0;
  }
  std::cerr << "[oj] server did not start cleanly (address already in use?)\n";
  return 1;
}