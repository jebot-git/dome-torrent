// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace torrent {
struct Row {
  std::string id, name, state, path, error, magnet;
  double progress = 0;
  std::int64_t done = 0, total = 0, downloaded = 0, uploaded = 0;
  int downRate = 0, upRate = 0, peers = 0, seeds = 0;
  bool metadata = false, paused = false;
  std::vector<std::string> files;
};
struct Snapshot {
  std::vector<Row> rows;
  std::vector<std::string> messages;
  bool stopped = false;
};
struct Options { std::filesystem::path state; bool localOnly = false; };
// All libtorrent and disk operations run on one worker; UI reads copied snapshots.
class Engine {
  struct Impl;
  std::unique_ptr<Impl> impl;
public:
  explicit Engine(Options);
  ~Engine();
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;
  void add(std::string source, std::filesystem::path destination, bool paused = false);
  void pause(std::string id, bool paused);
  void recheck(std::string id);
  void remove(std::string id); // Retains all downloaded payloads.
  void connectPeer(std::string id, unsigned short port); // Loopback integration fixture.
  void stop(); // Nonblocking; snapshot.stopped follows durable resume saves.
  Snapshot snapshot() const;
};
std::filesystem::path defaultState();
std::filesystem::path defaultDownloads();
}
