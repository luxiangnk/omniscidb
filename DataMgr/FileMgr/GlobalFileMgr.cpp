/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file        GlobalFileMgr.cpp
 * @author      Norair Khachiyan <norair@map-d.com>
 * @author      Todd Mostak <todd@map-d.com>
 */

#include "DataMgr/FileMgr/GlobalFileMgr.h"

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "DataMgr/ForeignStorage/ForeignStorageInterface.h"
#include "Shared/File.h"

using namespace std;

namespace File_Namespace {

GlobalFileMgr::GlobalFileMgr(const int deviceId,
                             std::string basePath,
                             const size_t num_reader_threads,
                             const size_t defaultPageSize)
    : AbstractBufferMgr(deviceId)
    , basePath_(basePath)
    , num_reader_threads_(num_reader_threads)
    , epoch_(-1)
    ,  // set the default epoch for all tables corresponding to the time of
       // last checkpoint
    defaultPageSize_(defaultPageSize) {
  mapd_db_version_ =
      1;  // DS changes triggered by individual FileMgr per table project (release 2.1.0)
  dbConvert_ = false;
  init();
}

void GlobalFileMgr::init() {
  // check if basePath_ already exists, and if not create one
  boost::filesystem::path path(basePath_);
  if (basePath_.size() > 0 && basePath_[basePath_.size() - 1] != '/') {
    basePath_.push_back('/');
  }
  if (boost::filesystem::exists(path)) {
    if (!boost::filesystem::is_directory(path)) {
      LOG(FATAL) << "Specified path is not a directory.";
    }
  } else {  // data directory does not exist
    if (!boost::filesystem::create_directory(path)) {
      LOG(FATAL) << "Could not create data directory";
    }
  }
}

void GlobalFileMgr::checkpoint() {
  mapd_unique_lock<mapd_shared_mutex> write_lock(fileMgrs_mutex_);
  for (auto fileMgrsIt = allFileMgrs_.begin(); fileMgrsIt != allFileMgrs_.end();
       ++fileMgrsIt) {
    fileMgrsIt->second->checkpoint();
  }
}

void GlobalFileMgr::checkpoint(const int db_id, const int tb_id) {
  getFileMgr(db_id, tb_id)->checkpoint();
}

size_t GlobalFileMgr::getNumChunks() {
  mapd_shared_lock<mapd_shared_mutex> read_lock(fileMgrs_mutex_);
  size_t num_chunks = 0;
  for (auto fileMgrsIt = allFileMgrs_.begin(); fileMgrsIt != allFileMgrs_.end();
       ++fileMgrsIt) {
    num_chunks += fileMgrsIt->second->getNumChunks();
  }

  return num_chunks;
}

void GlobalFileMgr::deleteBuffersWithPrefix(const ChunkKey& keyPrefix, const bool purge) {
  /* keyPrefix[0] can be -1 only for gpu or cpu buffers but not for FileMgr.
   * There is no assert here, as GlobalFileMgr is being called with -1 value as well in
   * the same loop with other buffers. So the case of -1 will just be ignored, as nothing
   * needs to be done.
   */
  if (keyPrefix[0] != -1) {
    return getFileMgr(keyPrefix)->deleteBuffersWithPrefix(keyPrefix, purge);
  }
}

AbstractBufferMgr* GlobalFileMgr::findFileMgr(const int db_id, const int tb_id) {
  // NOTE: only call this private function after locking is already in place
  AbstractBufferMgr* fm = nullptr;
  const auto file_mgr_key = std::make_pair(db_id, tb_id);
  if (auto it = allFileMgrs_.find(file_mgr_key); it != allFileMgrs_.end()) {
    fm = it->second;
  }
  return fm;
}

void GlobalFileMgr::deleteFileMgr(const int db_id, const int tb_id) {
  // NOTE: only call this private function after locking is already in place
  const auto file_mgr_key = std::make_pair(db_id, tb_id);
  if (auto it = ownedFileMgrs_.find(file_mgr_key); it != ownedFileMgrs_.end()) {
    ownedFileMgrs_.erase(it);
  }
  if (auto it = allFileMgrs_.find(file_mgr_key); it != allFileMgrs_.end()) {
    allFileMgrs_.erase(it);
  }
}

AbstractBufferMgr* GlobalFileMgr::getFileMgr(const int db_id, const int tb_id) {
  {  // check if FileMgr already exists for (db_id, tb_id)
    mapd_shared_lock<mapd_shared_mutex> read_lock(fileMgrs_mutex_);
    AbstractBufferMgr* fm = findFileMgr(db_id, tb_id);
    if (fm) {
      return fm;
    }
  }

  {  // create new FileMgr for (db_id, tb_id)
    mapd_unique_lock<mapd_shared_mutex> write_lock(fileMgrs_mutex_);
    AbstractBufferMgr* fm = findFileMgr(db_id, tb_id);
    if (fm) {
      return fm;  // mgr was added between the read lock and the write lock
    }
    const auto file_mgr_key = std::make_pair(db_id, tb_id);
    const auto foreign_buffer_manager =
        ForeignStorageInterface::lookupBufferManager(db_id, tb_id);
    if (foreign_buffer_manager) {
      CHECK(allFileMgrs_.insert(std::make_pair(file_mgr_key, foreign_buffer_manager))
                .second);
      return foreign_buffer_manager;
    } else {
      auto s = std::make_shared<FileMgr>(
          0, this, file_mgr_key, num_reader_threads_, epoch_, defaultPageSize_);
      CHECK(ownedFileMgrs_.insert(std::make_pair(file_mgr_key, s)).second);
      CHECK(allFileMgrs_.insert(std::make_pair(file_mgr_key, s.get())).second);
      return s.get();
    }
  }
}

void GlobalFileMgr::writeFileMgrData(
    FileMgr* fileMgr) {  // this function is not used, keep it for now for future needs
  mapd_shared_lock<mapd_shared_mutex> read_lock(fileMgrs_mutex_);
  for (auto fileMgrIt = allFileMgrs_.begin(); fileMgrIt != allFileMgrs_.end();
       fileMgrIt++) {
    FileMgr* fm = dynamic_cast<FileMgr*>(fileMgrIt->second);
    CHECK(fm);
    if ((fileMgr != 0) && (fileMgr != fm)) {
      continue;
    }
    for (auto chunkIt = fm->chunkIndex_.begin(); chunkIt != fm->chunkIndex_.end();
         chunkIt++) {
      chunkIt->second->write((int8_t*)chunkIt->second, chunkIt->second->size(), 0);
      // chunkIt->second->write((int8_t*)chunkIt->second, chunkIt->second->size(), 0,
      // CPU_LEVEL, -1);
    }
  }
}

void GlobalFileMgr::removeTableRelatedDS(const int db_id, const int tb_id) {
  mapd_unique_lock<mapd_shared_mutex> write_lock(fileMgrs_mutex_);
  auto fm = dynamic_cast<File_Namespace::FileMgr*>(findFileMgr(db_id, tb_id));
  if (fm) {
    fm->closeRemovePhysical();
  } else {
    // fileMgr has not been initialized so there is no need to
    // spend the time initializing
    // initialize just enough to have to rename
    const auto file_mgr_key = std::make_pair(db_id, tb_id);
    auto u = std::make_unique<FileMgr>(0, this, file_mgr_key, true);
    u->closeRemovePhysical();
  }
  // remove table related in-memory DS only if directory was removed successfully

  deleteFileMgr(db_id, tb_id);
}

void GlobalFileMgr::setTableEpoch(const int db_id,
                                  const int tb_id,
                                  const int start_epoch) {
  mapd_unique_lock<mapd_shared_mutex> write_lock(fileMgrs_mutex_);
  const auto file_mgr_key = std::make_pair(db_id, tb_id);
  // this is where the real rollback of any data ahead of the currently set epoch is
  // performed
  auto u = std::make_unique<FileMgr>(
      0, this, file_mgr_key, num_reader_threads_, start_epoch, defaultPageSize_);
  u->setEpoch(start_epoch - 1);
  // remove the dummy one we built
  u.reset();

  // see if one exists currently, and remove it
  deleteFileMgr(db_id, tb_id);
}

size_t GlobalFileMgr::getTableEpoch(const int db_id, const int tb_id) {
  auto fm = dynamic_cast<FileMgr*>(getFileMgr(db_id, tb_id));
  CHECK(fm);
  return fm->epoch_;
}

}  // namespace File_Namespace
