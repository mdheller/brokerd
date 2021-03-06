/**
 * This file is part of the "FnordMetric" project
 *   Copyright (c) 2014 Paul Asmuth, Google Inc.
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <iostream>
#include <regex>
#include <brokerd/util/logging.h>
#include <brokerd/util/fileutil.h>
#include <brokerd/channel_map.h>

namespace brokerd {

ReturnCode ChannelMap::openDirectory(
    const std::string& data_dir,
    std::unique_ptr<ChannelMap>* channel_map) try {
  if (!FileUtil::exists(data_dir) || !FileUtil::isDirectory(data_dir)) {
    return ReturnCode::errorf("EARG", "not a directory: $0", data_dir);
  }

  FileLock data_dir_lock(FileUtil::joinPaths(data_dir, "~lock"));
  data_dir_lock.lock();

  std::string hostid;
  auto hostid_file = FileUtil::joinPaths(data_dir, "~serverid");
  if (!FileUtil::exists(hostid_file)) {
    hostid = Random::singleton()->hex128();
    auto f = File::openFile(hostid_file + "~", File::O_CREATE | File::O_WRITE);
    f.write(hostid.data(), hostid.size());
    FileUtil::mv(hostid_file + "~", hostid_file);
  } else {
    hostid = FileUtil::read(hostid_file).toString();
  }

  channel_map->reset(
      new ChannelMap(
          data_dir,
          std::move(data_dir_lock),
          hostid));

  std::map<std::string, std::set<uint64_t>> file_map;
  FileUtil::ls(data_dir, [&file_map] (const auto& file) -> bool {
    std::regex regex("^([a-z0-9A-Z\\._-]+)~([0-9]+)$");
    std::smatch regex_match;

    if (std::regex_match(file, regex_match, regex)) {
      file_map[regex_match[1]].insert(std::stoull(regex_match[2]));
    }

    return true;
  });

  for (const auto& e : file_map) {
    auto channel_path = FileUtil::joinPaths(data_dir, e.first);

    std::list<ChannelSegment> segments;
    for (const auto& offset : e.second) {
      ChannelSegment s;
      auto rc = segmentReadHeader(channel_path, offset, &s);
      if (!rc.isSuccess()) {
        return rc;
      }

      segments.emplace_back(s);
    }

    std::shared_ptr<Channel> channel;
    auto rc = Channel::openChannel(channel_path, segments, &channel);
    if (!rc.isSuccess()) {
      return rc;
    }

    channel_map->get()->channels_.emplace(e.first, std::move(channel));
  }

  return ReturnCode::success();
} catch (const std::exception& e) {
  return ReturnCode::error(e);
}

ChannelMap::ChannelMap(
    const String& data_dir,
    FileLock&& data_dir_lock,
    const std::string& hostid) :
    data_dir_(data_dir),
    data_dir_lock_(std::move(data_dir_lock_)),
    hostid_(hostid) {}

ReturnCode ChannelMap::findChannel(
    const ChannelID& channel_id,
    bool create,
    std::shared_ptr<Channel>* channel) {
  std::unique_lock<std::mutex> lk(channels_mutex_);
  auto iter = channels_.find(channel_id.str());
  if (iter != channels_.end()) {
    *channel = iter->second;
    return ReturnCode::success();
  }

  if (!create) {
    return ReturnCode::errorf("EARG", "channel not found: $0", channel_id.str());
  }

  auto channel_path = FileUtil::joinPaths(data_dir_, channel_id.str());
  auto rc = Channel::createChannel(channel_path, channel);
  if (!rc.isSuccess()) {
    return rc;
  }

  channels_.emplace(channel_id.str(), *channel);
  return ReturnCode::success();
}

std::string ChannelMap::getUID() const noexcept {
  return hostid_;
}

} // namespace brokerd

