/// Copyright 2016 Pinterest Inc.
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
/// http://www.apache.org/licenses/LICENSE-2.0

/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.

//
// @kangnan li (kangnanli@pinterest.com)
//

#include <stdlib.h>
#include <ctime>
#include <iostream>
#include <thread>
#include <vector>

#include "boost/filesystem.hpp"
#include "gflags/gflags.h"
#include "gtest/gtest.h"
#include "rocksdb/db.h"
#include "rocksdb/sst_file_writer.h"
#include "rocksdb_admin/application_db.h"
#include "rocksdb_replicator/rocksdb_replicator.h"

DEFINE_bool(log_to_stdout, false, "Enable output some assisting logs to stdout");

namespace admin {

using boost::filesystem::remove_all;
using rocksdb::DB;
using rocksdb::DestroyDB;
using rocksdb::EnvOptions;
using rocksdb::FlushOptions;
using rocksdb::Logger;
using rocksdb::Options;
using rocksdb::Slice;
using rocksdb::SstFileWriter;
using rocksdb::WriteOptions;
using std::cout;
using std::endl;
using std::list;
using std::make_unique;
using std::pair;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::unique_ptr;
using std::vector;
using std::chrono::seconds;
using std::this_thread::sleep_for;

class ApplicationDBTestBase : public testing::Test {
public:
  ApplicationDBTestBase() {
    static thread_local unsigned int seed = time(nullptr);
    db_name_ = "test_db_" + to_string(rand_r(&seed));
    db_path_ = testDir() + "/" + db_name_;
    remove_all(db_path_);
    // default ApplicationDB with DB.allow_ingest_behind=false
    last_options_ = getDefaultOptions();
    Reopen(last_options_);
  }

  ~ApplicationDBTestBase(){};

  void createSstWithContent(const string& sst_filename, list<pair<string, string>>& key_vals) {
    EXPECT_NO_THROW(remove_all(sst_filename));

    Options options;
    SstFileWriter sst_file_writer(EnvOptions(), options, options.comparator);
    auto s = sst_file_writer.Open(sst_filename);
    ASSERT_TRUE(s.ok()) << s.ToString();

    list<pair<string, string>>::iterator it;
    for (it = key_vals.begin(); it != key_vals.end(); ++it) {
      s = sst_file_writer.Put(it->first, it->second);
      ASSERT_TRUE(s.ok()) << s.ToString();
    }

    s = sst_file_writer.Finish();
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  int numTableFilesAtLevel(int level) {
    std::string p;
    db_->rocksdb()->GetProperty("rocksdb.num-files-at-level" + std::to_string(level), &p);
    return atoi(p.c_str());
  }

  int numCompactPending() {
    uint64_t p;
    db_->rocksdb()->GetIntProperty("rocksdb.compaction-pending", &p);
    return static_cast<int>(p);
  }

  string levelStats() {
    std::string p;
    db_->rocksdb()->GetProperty("rocksdb.levelstats", &p);
    return p;
  }

  void Reopen(const Options& options) {
    close();
    last_options_ = options;
    DB* db;
    if (FLAGS_log_to_stdout) {
      cout << "Open DB with db_path: " << db_path_ << endl;
    }
    auto status = DB::Open(options, db_path_, &db);
    ASSERT_TRUE(status.ok()) << status.ToString();
    ASSERT_TRUE(db != nullptr);
    db_ = make_unique<ApplicationDB>(
        db_name_, shared_ptr<DB>(db), replicator::DBRole::SLAVE, nullptr);
  }

  void DestroyAndReopen(const Options& options) {
    Destroy(last_options_);
    Reopen(options);
  }

  void Destroy(const Options& options) {
    close();
    ASSERT_TRUE(DestroyDB(db_path_, options).ok());
  }

  void close() {
    if (db_ != nullptr) {
      db_.reset(nullptr);
    }
  }

  const string testDir() { return "/tmp"; }

  Options getDefaultOptions() {
    Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.WAL_size_limit_MB = 100;
    return options;
  }

public:
  unique_ptr<ApplicationDB> db_;
  string db_name_;
  string db_path_;
  Options last_options_;
};

TEST_F(ApplicationDBTestBase, SetOptionsAndTakeEffect) {
  // Control: default has auto compaction on
  EXPECT_FALSE(last_options_.disable_auto_compactions);

  // Verify: lastest Options returned after SetOptions
  db_->rocksdb()->SetOptions({{"disable_auto_compactions", "true"}});
  // setOptions won't update cached last_options_ at Test
  EXPECT_FALSE(last_options_.disable_auto_compactions);
  EXPECT_TRUE(db_->rocksdb()->GetOptions().disable_auto_compactions);

  // Verify: db's options reload when reopen
  DestroyAndReopen(getDefaultOptions());
  Options options = last_options_;
  options.error_if_exists = false;  // It is set true by default at Test
  options.disable_auto_compactions = true;
  Reopen(options);
  EXPECT_TRUE(last_options_.disable_auto_compactions);
  EXPECT_TRUE(db_->rocksdb()->GetOptions().disable_auto_compactions);
}

TEST_F(ApplicationDBTestBase, SetOptionsDisableEnableAutoCompaction) {
  // Control: verify auto compaction execution when level0_file_num reach config
  Options options = getDefaultOptions();
  options.error_if_exists = false;
  options.level0_file_num_compaction_trigger = 1;
  Reopen(options);
  EXPECT_EQ(numTableFilesAtLevel(1), 0);
  for (int i = 0; i < 5; ++i) {
    db_->rocksdb()->Put(WriteOptions(), to_string(i), to_string(i));
  }
  db_->rocksdb()->Flush(FlushOptions());
  if (FLAGS_log_to_stdout) {
    cout << "Level Stats Right After Flush 1st sst: \n" << levelStats() << endl;
  }
  // After flush 1st sst into L0, an auto compaction will be triggered. Ideally, we should wait the
  // for compaction complete. But, the API is not avail. Thus, here we wait for 1s for compaction
  // finish
  sleep_for(seconds(1));
  if (FLAGS_log_to_stdout) {
    cout << "Level Stats After Wait(1s) for Compaction: \n" << levelStats() << endl;
  }
  EXPECT_EQ(numTableFilesAtLevel(0), 0);
  EXPECT_EQ(numTableFilesAtLevel(1), 1);

  // Verify auto compaction paused after setOptions
  db_->rocksdb()->SetOptions({{"disable_auto_compactions", "true"}});
  for (int i = 5; i < 10; ++i) {
    db_->rocksdb()->Put(WriteOptions(), to_string(i), to_string(i));
  }
  db_->rocksdb()->Flush(FlushOptions());
  sleep_for(seconds(1));  // wait for 1s for compaction finish if exist
  if (FLAGS_log_to_stdout) {
    cout << "Level Stats after flush a 2nd sst, with auto compaction disabled: \n"
         << levelStats() << endl;
  }
  EXPECT_EQ(numTableFilesAtLevel(0), 1);
  EXPECT_EQ(numTableFilesAtLevel(1), 1);
  for (int i = 10; i < 15; ++i) {
    db_->rocksdb()->Put(WriteOptions(), to_string(i), to_string(i));
  }
  db_->rocksdb()->Flush(FlushOptions());
  sleep_for(seconds(1));  // wait for 1s for compaction finish if exist
  if (FLAGS_log_to_stdout) {
    cout << "Level Stats after flush a 3rd sst, with auto compaction disabled: \n"
         << levelStats() << endl;
  }
  EXPECT_EQ(numTableFilesAtLevel(0), 2);
  EXPECT_EQ(numTableFilesAtLevel(1), 1);
  // with auto compact disabled, numCompactPending is still calculated
  EXPECT_EQ(numCompactPending(), 1);

  // Verify the compaction continue after enable auto compaction again.
  // in db.h, it claims that EnableAutoCompaction will first set options and then schedule
  // a flush/compaction; but, the impl only has the 1st step of setting options.
  // (https://github.com/facebook/rocksdb/blob/cf160b98e1a9bd7b45f115337a923e6b6da7d9c2/db/db_impl/db_impl_compaction_flush.cc#L2142).
  // db_->rocksdb()->EnableAutoCompaction({db_->rocksdb()->DefaultColumnFamily()});
  // from our test, SetOptions deliver expected result.
  db_->rocksdb()->SetOptions({{"disable_auto_compactions", "false"}});
  sleep_for(seconds(1));  // wait for 1s for compaction finish if exist
  if (FLAGS_log_to_stdout) {
    cout << "Level Stats after enable auto compaction again \n" << levelStats() << endl;
  }
  EXPECT_EQ(numTableFilesAtLevel(0), 0);
  EXPECT_EQ(numTableFilesAtLevel(1), 2);  // the two sst at L0 will merge into 1 at L1
  EXPECT_EQ(numCompactPending(), 0);
}

TEST_F(ApplicationDBTestBase, GetLSMLevelInfo) {
  // Verify: DB level=7 at new create
  EXPECT_EQ(db_->rocksdb()->NumberLevels(), 7);
  // level num: 0, 1, ..., 6
  EXPECT_EQ(db_->getHighestEmptyLevel(), 6);

  string sst_file1 = "/tmp/file1.sst";
  list<pair<string, string>> sst1_content = {{"1", "1"}, {"2", "2"}};
  createSstWithContent(sst_file1, sst1_content);

  // ingest sst files, and try to read the current occupied LSM level agin
  rocksdb::IngestExternalFileOptions ifo;
  ifo.move_files = true;
  ifo.allow_global_seqno = true;
  ifo.ingest_behind = true;
  EXPECT_FALSE(db_->rocksdb()->GetOptions().allow_ingest_behind);
  auto s = db_->rocksdb()->IngestExternalFile({sst_file1}, ifo);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(db_->getHighestEmptyLevel(), 6);

  auto options = getDefaultOptions();
  options.allow_ingest_behind = true;
  DestroyAndReopen(options);

  ifo.ingest_behind = true;
  EXPECT_TRUE(db_->rocksdb()->GetOptions().allow_ingest_behind);
  s = db_->rocksdb()->IngestExternalFile({sst_file1}, ifo);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(db_->getHighestEmptyLevel(), 5);  // level6 is occupied by ingested data

  // compact DB
  rocksdb::CompactRangeOptions compact_options;
  compact_options.change_level = false;
  // if change_level is false (default), compacted data will move to bottommost
  db_->rocksdb()->CompactRange(compact_options, nullptr, nullptr);
  EXPECT_EQ(db_->getHighestEmptyLevel(), 5);

  compact_options.change_level = true;
  // if change_level is false (default), compacted data will move to bottommost
  db_->rocksdb()->CompactRange(compact_options, nullptr, nullptr);
  EXPECT_EQ(db_->getHighestEmptyLevel(), 6);
}

}  // namespace admin

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}