/*
 * Copyright 2020 OmniSci, Inc.
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
 * @file ForeignTableDmlTest.cpp
 * @brief Test suite for DML SQL queries on foreign tables
 */

#include <gtest/gtest.h>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "Archive/S3Archive.h"
#include "DBHandlerTestHelpers.h"
#include "DataMgr/ForeignStorage/ForeignStorageCache.h"
#include "DataMgr/ForeignStorage/ForeignTableRefresh.h"
#include "Geospatial/Types.h"
#include "ImportExport/DelimitedParserUtils.h"
#include "TestHelpers.h"

#ifndef BASE_PATH
#define BASE_PATH "./tmp"
#endif

extern bool g_enable_fsi;
std::string test_binary_file_path;

namespace bp = boost::process;
namespace bf = boost::filesystem;
using path = bf::path;

static const std::string default_table_name = "test_foreign_table";

/**
 * Helper class for creating foreign tables
 */
class ForeignTableTest : public DBHandlerTestFixture {
 protected:
  void SetUp() override { DBHandlerTestFixture::SetUp(); }
  void TearDown() override { DBHandlerTestFixture::TearDown(); }
  static std::string getCreateForeignTableQuery(const std::string& columns,
                                                const std::string& file_name_base,
                                                const std::string& data_wrapper_type,
                                                const int table_number = 0) {
    return getCreateForeignTableQuery(
        columns, {}, file_name_base, data_wrapper_type, table_number);
  }

  static std::string getCreateForeignTableQuery(
      const std::string& columns,
      const std::map<std::string, std::string> options,
      const std::string& file_name_base,
      const std::string& data_wrapper_type,
      const int table_number = 0,
      const std::string& table_name = default_table_name,
      const std::string extension = "") {
    std::string query{"CREATE FOREIGN TABLE " + table_name};
    if (table_number) {
      query += "_" + std::to_string(table_number);
    }

    std::string filename = file_name_base;
    if (extension == "dir") {
      filename += "_" + data_wrapper_type + "_dir";
    } else if (extension.empty()) {
      filename += "." + data_wrapper_type;
    } else {
      filename += "." + extension;
    }

    query += " " + columns + " SERVER omnisci_local_" + data_wrapper_type +
             " WITH (file_path = '" + getDataFilesPath() + filename + "'";
    for (auto& [key, value] : options) {
      query += ", " + key + " = '" + value + "'";
    }
    query += ");";
    return query;
  }

  static std::string getDataFilesPath() {
    return bf::canonical(test_binary_file_path + "/../../Tests/FsiDataFiles").string() +
           "/";
  }

  static void sqlCreateForeignTable(const std::string& columns,
                                    const std::string& file_name,
                                    const std::string& data_wrapper_type,
                                    const std::map<std::string, std::string> options = {},
                                    const int table_number = 0,
                                    const std::string& table_name = default_table_name) {
    sqlDropForeignTable(table_number, table_name);
    auto query = getCreateForeignTableQuery(
        columns, options, file_name, data_wrapper_type, table_number, table_name);
    sql(query);
  }

  static void sqlDropForeignTable(const int table_number = 0,
                                  const std::string& table_name = default_table_name) {
    std::string query{"DROP FOREIGN TABLE IF EXISTS " + table_name};
    if (table_number != 0) {
      query += "_" + std::to_string(table_number);
    }
    sql(query);
  }

  static ChunkKey getChunkKeyFromTable(const Catalog_Namespace::Catalog& cat,
                                       const std::string& table_name,
                                       const ChunkKey& key_suffix) {
    const TableDescriptor* fd = cat.getMetadataForTable(table_name);
    ChunkKey key{cat.getCurrentDB().dbId, fd->tableId};
    for (auto i : key_suffix) {
      key.push_back(i);
    }
    return key;
  }
};

class SelectQueryTest : public ForeignTableTest {
 protected:
  void SetUp() override {
    ForeignTableTest::SetUp();
    import_export::delimited_parser::set_max_buffer_resize(max_buffer_resize_);
    sql("DROP FOREIGN TABLE IF EXISTS test_foreign_table;");
    sql("DROP FOREIGN TABLE IF EXISTS test_foreign_table_2;");
    sql("DROP SERVER IF EXISTS test_server;");
  }

  void TearDown() override {
    sql("DROP FOREIGN TABLE IF EXISTS test_foreign_table;");
    sql("DROP FOREIGN TABLE IF EXISTS test_foreign_table_2;");
    sql("DROP SERVER IF EXISTS test_server;");
    ForeignTableTest::TearDown();
  }

  template <typename T>
  inline static std::unique_ptr<ChunkMetadata> createChunkMetadata(
      const int column_id,
      const size_t num_bytes,
      const size_t num_elements,
      const T& min,
      const T& max,
      bool has_nulls,
      const std::string& table_name) {
    auto chunk_metadata =
        createChunkMetadata(column_id, num_bytes, num_elements, has_nulls, table_name);
    if (chunk_metadata->sqlType.is_array()) {
      auto saved_sql_type = chunk_metadata->sqlType;
      chunk_metadata->sqlType = saved_sql_type.get_elem_type();
      chunk_metadata->fillChunkStats(min, max, has_nulls);
      chunk_metadata->sqlType = saved_sql_type;
    } else {
      chunk_metadata->fillChunkStats(min, max, has_nulls);
    }
    return chunk_metadata;
  }

  template <typename T>
  inline static std::unique_ptr<ChunkMetadata> createChunkMetadata(
      const int column_id,
      const size_t num_bytes,
      const size_t num_elements,
      const T& min,
      const T& max,
      bool has_nulls) {
    return createChunkMetadata(
        column_id, num_bytes, num_elements, min, max, has_nulls, "test_foreign_table");
  }

  inline static std::unique_ptr<ChunkMetadata> createChunkMetadata(
      const int column_id,
      const size_t num_bytes,
      const size_t num_elements,
      bool has_nulls,
      const std::string& table_name) {
    auto& cat = getCatalog();
    auto foreign_table = cat.getMetadataForTable(table_name);
    auto column_descriptor = cat.getMetadataForColumn(foreign_table->tableId, column_id);
    auto chunk_metadata = std::make_unique<ChunkMetadata>();
    chunk_metadata->sqlType = column_descriptor->columnType;
    chunk_metadata->numElements = num_elements;
    chunk_metadata->numBytes = num_bytes;
    chunk_metadata->chunkStats.has_nulls = has_nulls;
    chunk_metadata->chunkStats.min.stringval = nullptr;
    chunk_metadata->chunkStats.max.stringval = nullptr;
    return chunk_metadata;
  }

  inline static std::unique_ptr<ChunkMetadata> createChunkMetadata(
      const int column_id,
      const size_t num_bytes,
      const size_t num_elements,
      bool has_nulls) {
    return createChunkMetadata(
        column_id, num_bytes, num_elements, has_nulls, "test_foreign_table");
  }

  void assertExpectedChunkMetadata(
      const std::map<std::pair<int, int>, std::unique_ptr<ChunkMetadata>>&
          expected_metadata) const {
    assertExpectedChunkMetadata(expected_metadata, "test_foreign_table");
  }

  void assertExpectedChunkMetadata(
      const std::map<std::pair<int, int>, std::unique_ptr<ChunkMetadata>>&
          expected_metadata,
      const std::string& table_name) const {
    auto& cat = getCatalog();
    auto foreign_table = cat.getMetadataForTable(table_name);
    if (!foreign_table) {
      throw std::runtime_error("Could not find foreign table: " + table_name);
    }
    auto fragmenter = foreign_table->fragmenter;
    if (!fragmenter) {
      throw std::runtime_error("Fragmenter does not exist for foreign table: " +
                               table_name);
    }
    std::map<std::pair<int, int>, bool> expected_metadata_found;
    for (auto& [k, v] : expected_metadata) {
      expected_metadata_found[k] = false;
    }

    auto query_info = fragmenter->getFragmentsForQuery();
    for (const auto& fragment : query_info.fragments) {
      auto& chunk_metadata_map = fragment.getChunkMetadataMapPhysical();
      for (auto& [col_id, chunk_metadata] : chunk_metadata_map) {
        auto fragment_id = fragment.fragmentId;
        auto column_id = col_id;
        auto fragment_column_ids = std::make_pair(fragment_id, column_id);
        auto expected_metadata_iter = expected_metadata.find(fragment_column_ids);
        ASSERT_NE(expected_metadata_iter, expected_metadata.end())
            << boost::format(
                   "Foreign table chunk metadata not found in expected metadata: "
                   "fragment_id: %d, column_id: %d") %
                   fragment_id % column_id;
        expected_metadata_found[fragment_column_ids] = true;
        ASSERT_EQ(*chunk_metadata, *expected_metadata_iter->second)
            << boost::format("At fragment_id: %d, column_id: %d") % fragment_id %
                   column_id;
      }
    }

    for (auto& [k, v] : expected_metadata_found) {
      auto fragment_id = k.first;
      auto column_id = k.second;
      if (!v) {
        ASSERT_TRUE(false) << boost::format(
                                  "Expected chunk metadata not found in foreign table "
                                  "metadata: fragment_id: %d, column_id: %d") %
                                  fragment_id % column_id;
      }
    }
  }

  inline static size_t max_buffer_resize_ =
      import_export::delimited_parser::get_max_buffer_resize();
};

class CacheControllingSelectQueryTest
    : public SelectQueryTest,
      public ::testing::WithParamInterface<DiskCacheLevel> {
 public:
  inline static std::string cache_path_ = to_string(BASE_PATH) + "/omnisci_disk_cache/";
  DiskCacheLevel starting_cache_level_;

 protected:
  void resetPersistentStorageMgr(DiskCacheLevel cache_level) {
    for (auto table_it : getCatalog().getAllTableMetadata()) {
      getCatalog().removeFragmenterForTable(table_it->tableId);
    }
    getCatalog().getDataMgr().resetPersistentStorage(
        {cache_path_, cache_level}, 0, getSystemParameters());
  }

  void SetUp() override {
    // Disable/enable the cache as test param requires
    starting_cache_level_ = getCatalog()
                                .getDataMgr()
                                .getPersistentStorageMgr()
                                ->getDiskCacheConfig()
                                .enabled_level;
    if (starting_cache_level_ != GetParam()) {
      resetPersistentStorageMgr(GetParam());
    }
    SelectQueryTest::SetUp();
  }

  void TearDown() override {
    SelectQueryTest::TearDown();
    // Reset cache to pre-test conditions
    if (starting_cache_level_ != GetParam()) {
      resetPersistentStorageMgr(starting_cache_level_);
    }
  }
};

// compare files, adjusting for basepath
bool compare_json_files(const std::string& generated,
                        const std::string& reference,
                        const std::string& basepath) {
  std::ifstream gen_file(generated);
  std::ifstream ref_file(reference);
  // Compare each file line by line
  while (gen_file && ref_file) {
    std::string gen_line;
    std::getline(gen_file, gen_line);
    std::string ref_line;
    std::getline(ref_file, ref_line);
    boost::replace_all(gen_line, basepath, "BASEPATH/");
    boost::algorithm::trim(gen_line);
    boost::algorithm::trim(ref_line);
    if (gen_line.compare(ref_line) != 0) {
      std::cout << "Mismatched json line \n";
      std::cout << gen_line << "\n";
      return false;
    }
  }
  if (ref_file || gen_file) {
    // # of lines mismatch
    return false;
  }
  return true;
}

class RecoverCacheQueryTest : public ForeignTableTest {
 public:
  inline static std::string cache_path_ = to_string(BASE_PATH) + "/omnisci_disk_cache/";
  Catalog_Namespace::Catalog* cat_;
  PersistentStorageMgr* psm_;
  foreign_storage::ForeignStorageCache* cache_;

 protected:
  void resetPersistentStorageMgr(DiskCacheLevel cache_level) {
    for (auto table_it : cat_->getAllTableMetadata()) {
      cat_->removeFragmenterForTable(table_it->tableId);
    }
    cat_->getDataMgr().resetPersistentStorage(
        {cache_path_, cache_level}, 0, getSystemParameters());
    psm_ = cat_->getDataMgr().getPersistentStorageMgr();
    cache_ = psm_->getDiskCache();
  }

  bool isTableDatawrapperRestored(const std::string& name) {
    auto td = getCatalog().getMetadataForTable(name);
    ChunkKey table_key{getCatalog().getCurrentDB().dbId, td->tableId};
    return getCatalog()
        .getDataMgr()
        .getPersistentStorageMgr()
        ->getForeignStorageMgr()
        ->isDatawrapperRestored(table_key);
  }

  bool isTableDatawrapperDataOnDisk(const std::string& name) {
    auto td = getCatalog().getMetadataForTable(name);
    ChunkKey table_key{getCatalog().getCurrentDB().dbId, td->tableId};
    return bf::exists(getCatalog()
                          .getDataMgr()
                          .getPersistentStorageMgr()
                          ->getDiskCache()
                          ->getCacheDirectoryForTablePrefix(table_key) +
                      "/wrapper_metadata.json");
  }

  bool compareTableDatawrapperMetadataToFile(const std::string& name,
                                             const std::string& filepath) {
    auto td = getCatalog().getMetadataForTable(name);
    ChunkKey table_key{getCatalog().getCurrentDB().dbId, td->tableId};
    return compare_json_files(getCatalog()
                                      .getDataMgr()
                                      .getPersistentStorageMgr()
                                      ->getDiskCache()
                                      ->getCacheDirectoryForTablePrefix(table_key) +
                                  "/wrapper_metadata.json",
                              filepath,
                              getDataFilesPath());
  }

  void resetStorageManagerAndClearTableMemory(const ChunkKey& table_key) {
    // Reset cache and clear memory representations.
    resetPersistentStorageMgr(DiskCacheLevel::fsi);
    cat_->getDataMgr().deleteChunksWithPrefix(table_key, MemoryLevel::CPU_LEVEL);
    cat_->getDataMgr().deleteChunksWithPrefix(table_key, MemoryLevel::GPU_LEVEL);
  }

  std::string getWrapperMetadataPath(const std::string& prefix,
                                     const std::string& data_wrapper_type = {}) {
    std::string path = getDataFilesPath() + "/wrapper_metadata/" + prefix;
    if (!data_wrapper_type.empty()) {
      path += "_" + data_wrapper_type;
    }
    return path + ".json";
  }

  void SetUp() override {
    DBHandlerTestFixture::SetUp();
    cat_ = &getCatalog();
    psm_ = cat_->getDataMgr().getPersistentStorageMgr();
    cache_ = psm_->getDiskCache();
  }

  void TearDown() override { DBHandlerTestFixture::TearDown(); }
  static void SetUpTestSuite() {}
  static void TearDownTestSuite() {}
};

class DataWrapperSelectQueryTest : public SelectQueryTest,
                                   public ::testing::WithParamInterface<std::string> {};

struct DataTypeFragmentSizeAndDataWrapperParam {
  int fragment_size;
  std::string wrapper;
  std::string extension;
};

struct AppendRefreshTestParam {
  int fragment_size;
  std::string wrapper;
  std::string filename;
  std::string file_display;
  bool recover_cache;
};

class DataTypeFragmentSizeAndDataWrapperTest
    : public SelectQueryTest,
      public testing::WithParamInterface<DataTypeFragmentSizeAndDataWrapperParam> {};

class RowGroupAndFragmentSizeSelectQueryTest
    : public SelectQueryTest,
      public ::testing::WithParamInterface<std::pair<int64_t, int64_t>> {};

namespace {
struct PrintToStringParamName {
  template <class ParamType>
  std::string operator()(const ::testing::TestParamInfo<ParamType>& info) const {
    std::stringstream ss;
    ss << info.param;
    return ss.str();
  }

  std::string operator()(
      const ::testing::TestParamInfo<std::pair<int, std::string>>& info) const {
    std::stringstream ss;
    ss << "Fragment_size_" << info.param.first << "_Data_wrapper_" << info.param.second;
    return ss.str();
  }

  std::string operator()(
      const ::testing::TestParamInfo<DataTypeFragmentSizeAndDataWrapperParam>& info)
      const {
    std::stringstream ss;
    ss << "Fragment_size_" << info.param.fragment_size << "_Data_wrapper_"
       << info.param.wrapper << "_Extension_" << info.param.extension;

    return ss.str();
  }

  std::string operator()(
      const ::testing::TestParamInfo<AppendRefreshTestParam>& info) const {
    std::stringstream ss;
    ss << "Fragment_size_" << info.param.fragment_size << "_Data_wrapper_"
       << info.param.wrapper << "_file_" << info.param.file_display
       << (info.param.recover_cache ? "_recover" : "");
    return ss.str();
  }
  std::string operator()(
      const ::testing::TestParamInfo<std::pair<int64_t, int64_t>>& info) const {
    std::stringstream ss;
    ss << "Rowgroup_size_" << info.param.first << "_Fragment_size_" << info.param.second;
    return ss.str();
  }
  std::string operator()(
      const ::testing::TestParamInfo<std::pair<std::string, std::string>>& info) const {
    std::stringstream ss;
    ss << "File_type_" << info.param.second;
    return ss.str();
  }
  std::string operator()(const ::testing::TestParamInfo<TExecuteMode::type>& info) const {
    std::stringstream ss;
    ss << ((info.param == TExecuteMode::GPU) ? "GPU" : "CPU");
    return ss.str();
  }
  std::string operator()(const ::testing::TestParamInfo<DiskCacheLevel>& info) const {
    std::stringstream ss;
    // clang-format off
    if (info.param == DiskCacheLevel::none) ss << "NoCache";
    if (info.param == DiskCacheLevel::fsi) ss << "FsiCache";
    if (info.param == DiskCacheLevel::non_fsi) ss << "NonFsiCache";
    if (info.param == DiskCacheLevel::all) ss << "AllCache";
    // clang-format on
    return ss.str();
  }
};
}  // namespace

TEST_P(CacheControllingSelectQueryTest, CustomServer) {
  sql("CREATE SERVER test_server FOREIGN DATA WRAPPER omnisci_csv "s +
      "WITH (storage_type = 'LOCAL_FILE', base_path = '" + getDataFilesPath() + "');");
  sql("CREATE FOREIGN TABLE test_foreign_table (t TEXT, i INTEGER[]) "
      "SERVER test_server WITH (file_path = 'example_1.csv');");
  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{"a", array({i(1), i(1), i(1)})},
                        {"aa", array({Null_i, i(2), i(2)})},
                        {"aaa", array({i(3), Null_i, i(3)})}},
                       result);
}

TEST_P(CacheControllingSelectQueryTest, DefaultLocalCsvServer) {
  std::string query = "CREATE FOREIGN TABLE test_foreign_table (t TEXT, i INTEGER[]) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "/example_1.csv');";
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{"a", array({i(1), i(1), i(1)})},
                        {"aa", array({Null_i, i(2), i(2)})},
                        {"aaa", array({i(3), Null_i, i(3)})}},
                       result);
}

TEST_P(CacheControllingSelectQueryTest, DefaultLocalParquetServer) {
  std::string query =
      "CREATE FOREIGN TABLE test_foreign_table (t TEXT, i INTEGER, f DOUBLE) "s +
      "SERVER omnisci_local_parquet WITH (file_path = '" + getDataFilesPath() +
      "/example_2.parquet');";
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{"a", i(1), 1.1},
                        {"aa", i(1), 1.1},
                        {"aa", i(2), 2.2},
                        {"aaa", i(1), 1.1},
                        {"aaa", i(2), 2.2},
                        {"aaa", i(3), 3.3}},
                       result);
}

// Create table with multiple fragments with file buffers less than size of a
// fragment Includes both fixed and variable length data
TEST_P(CacheControllingSelectQueryTest, MultipleDataBlocksPerFragment) {
  const auto& query =
      getCreateForeignTableQuery("(i INTEGER,  txt TEXT, txt_2 TEXT ENCODING NONE)",
                                 {{"buffer_size", "25"}, {"fragment_size", "64"}},
                                 "0_255",
                                 "csv");
  sql(query);

  // Check that data is correct
  {
    std::vector<std::vector<TargetValue>> expected_result_set;
    for (int number = 0; number < 256; number++) {
      expected_result_set.push_back(
          {i(number), std::to_string(number), std::to_string(number)});
    }
    TQueryResult result;
    sql(result, "SELECT * FROM test_foreign_table ORDER BY i;");
    assertResultSetEqual(expected_result_set, result);
  }

  // Check that WHERE statements filter numerical data correctly
  {
    std::vector<std::vector<TargetValue>> expected_result_set;
    for (int number = 128; number < 256; number++) {
      expected_result_set.push_back(
          {i(number), std::to_string(number), std::to_string(number)});
    }
    TQueryResult result;
    sql(result, "SELECT * FROM test_foreign_table  WHERE i >= 128 ORDER BY i;");
    assertResultSetEqual(expected_result_set, result);
  }
  {
    std::vector<std::vector<TargetValue>> expected_result_set;
    for (int number = 0; number < 128; number++) {
      expected_result_set.push_back(
          {i(number), std::to_string(number), std::to_string(number)});
    }
    TQueryResult result;
    sql(result, "SELECT * FROM test_foreign_table  WHERE i < 128 ORDER BY i;");
    assertResultSetEqual(expected_result_set, result);
  }
}

TEST_P(CacheControllingSelectQueryTest, ParquetNullRowgroups) {
  const auto& query =
      getCreateForeignTableQuery("(a SMALLINT, b SMALLINT)", "null_columns", "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  // clang-format off
  assertResultSetEqual({{i(NULL_SMALLINT),i(1)},
                        {i(NULL_SMALLINT),i(2)},
                        {i(NULL_SMALLINT),i(NULL_SMALLINT)},
                        {i(NULL_SMALLINT),i(NULL_SMALLINT)}},
                       result);
  // clang-format on
}

TEST_P(CacheControllingSelectQueryTest, CacheExists) {
  auto cache = getCatalog().getDataMgr().getPersistentStorageMgr()->getDiskCache();
  if (GetParam() == DiskCacheLevel::none) {
    ASSERT_EQ(cache, nullptr);
  } else {
    ASSERT_NE(cache, nullptr);
  }
}

TEST_P(CacheControllingSelectQueryTest, RefreshDisabledCache) {
  std::string temp_file{getDataFilesPath() + "/.tmp.csv"};
  bf::copy_file(
      getDataFilesPath() + "0.csv", temp_file, bf::copy_option::overwrite_if_exists);
  std::string query = "CREATE FOREIGN TABLE test_foreign_table (i INTEGER) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" + temp_file + "');";
  sql(query);
  TQueryResult pre_refresh_result;
  sql(pre_refresh_result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{i(0)}}, pre_refresh_result);
  bf::copy_file(getDataFilesPath() + "two_row_3_4.csv",
                temp_file,
                bf::copy_option::overwrite_if_exists);

  sql("REFRESH FOREIGN TABLES test_foreign_table;");
  TQueryResult post_refresh_result;
  sql(post_refresh_result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{i(3)}, {i(4)}}, post_refresh_result);
  bf::remove_all(temp_file);
}

TEST_F(SelectQueryTest, ParquetStringsAllNullPlacementPermutations) {
  const auto& query = getCreateForeignTableQuery(
      "( id INT, txt1 TEXT ENCODING NONE, txt2 TEXT ENCODING DICT (32), txt3 TEXT "
      "ENCODING DICT (16), txt4 TEXT ENCODING DICT (8))",
      "strings_with_all_null_placement_permutations",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table ORDER BY id;");
  // clang-format off
  assertResultSetEqual(
      {
        { i(1), "txt1", "txt1", "txt1", "txt1"},
        { i(2), "txt2", "txt2", "txt2", "txt2"},
        { i(3), "txt3", "txt3", "txt3", "txt3"},
        { i(4), "", "", "", "" },
        { i(5), "txt5", "txt5", "txt5", "txt5"},
        { i(6), "txt6", "txt6", "txt6", "txt6"},
        { i(7), "txt7", "txt7", "txt7", "txt7"},
        { i(8), "", "", "", "" },
        { i(9), "txt9", "txt9", "txt9", "txt9"},
        { i(10), "txt10", "txt10", "txt10", "txt10"},
        { i(11), "txt11", "txt11", "txt11", "txt11"},
        { i(12), "", "", "", "" },
        { i(13), "", "", "", "" },
        { i(14), "", "", "", "" },
        { i(15), "txt15", "txt15", "txt15", "txt15"},
        { i(16), "", "", "", "" },
        { i(17), "txt17", "txt17", "txt17", "txt17"},
        { i(18), "", "", "", "" },
        { i(19), "txt19", "txt19", "txt19", "txt19"},
        { i(20), "", "", "", "" },
        { i(21), "", "", "", "" },
        { i(22), "", "", "", "" },
        { i(23), "", "", "", "" },
        { i(24), "", "", "", "" }
      },
      result);
  // clang-format on
}

TEST_F(SelectQueryTest, DISABLED_ParquetStringDictionaryEncodedMetadataTest) {
  // TODO: This test fails, it highlights a major issue with loading
  // dictionaries for dict encoded strings upon chunk load time: only an empty
  // dictionary exists during the first query, thus any comparisons to fixed
  // string literals will fail until the dictionary exits.

  const auto& query = getCreateForeignTableQuery("(txt TEXT ENCODING DICT (32) )",
                                                 {{"fragment_size", "4"}},
                                                 "strings_repeating",
                                                 "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT count(txt) from test_foreign_table WHERE txt = 'a';");
  assertResultSetEqual({{
                           i(5),
                       }},
                       result);
}

TEST_F(SelectQueryTest, ParquetStringDictionaryEncodedMetadataTestAfterChunkLoad) {
  const auto& query = getCreateForeignTableQuery("(txt TEXT ENCODING DICT (32) )",
                                                 {{"fragment_size", "4"}},
                                                 "strings_repeating",
                                                 "parquet");
  sql(query);

  // Update the metadata of the string dictionary encoded column with the first
  // query
  sql("SELECT count(txt) from test_foreign_table WHERE txt = 'a';");
  TQueryResult result;
  sql(result, "SELECT count(txt) from test_foreign_table WHERE txt = 'a';");
  assertResultSetEqual({{
                           i(5),
                       }},
                       result);
}

TEST_F(SelectQueryTest, ParquetNumericAndBooleanTypesWithAllNullPlacementPermutations) {
  const auto& query = getCreateForeignTableQuery(
      "( id INT, bool BOOLEAN, i8 TINYINT, u8 SMALLINT, i16 SMALLINT, "
      "u16 INT, i32 INT, u32 BIGINT, i64 BIGINT, f32 FLOAT, "
      "f64 DOUBLE, fixedpoint DECIMAL(10,5) )",
      "numeric_and_boolean_types_with_all_null_placement_permutations",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table order by id;");

  // clang-format off
  assertResultSetEqual({
   {i(1),i(True),i(100),i(100),i(23000),i(23000),i(2047483647),i(2047483647),i(9123372036854775807),(1e-4f),(1e-4),(1.123)},
   {i(2),i(False),i(-127),i(0),i(-32767),i(0),i(-2147483647),i(0),i(-9223372036854775807),(3.141592f),(3.141592653589793),(100.1)},
   {i(3),i(True),i(127),i(255),i(32767),i(65535),i(2147483647),i(4294967295),i(9223372036854775807),(1e9f),(1e19),(2.22)},
   {i(4),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(5),i(False),i(-127),i(0),i(-32767),i(0),i(-2147483647),i(0),i(-9223372036854775807),(3.141592f),(3.141592653589793),(100.1)},
   {i(6),i(True),i(127),i(255),i(32767),i(65535),i(2147483647),i(4294967295),i(9223372036854775807),(1e9f),(1e19),(2.22)},
   {i(7),i(True),i(100),i(100),i(23000),i(23000),i(2047483647),i(2047483647),i(9123372036854775807),(1e-4f),(1e-4),(1.123)},
   {i(8),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(9),i(True),i(127),i(255),i(32767),i(65535),i(2147483647),i(4294967295),i(9223372036854775807),(1e9f),(1e19),(2.22)},
   {i(10),i(True),i(100),i(100),i(23000),i(23000),i(2047483647),i(2047483647),i(9123372036854775807),(1e-4f),(1e-4),(1.123)},
   {i(11),i(False),i(-127),i(0),i(-32767),i(0),i(-2147483647),i(0),i(-9223372036854775807),(3.141592f),(3.141592653589793),(100.1)},
   {i(12),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(13),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(14),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(15),i(True),i(127),i(255),i(32767),i(65535),i(2147483647),i(4294967295),i(9223372036854775807),(1e9f),(1e19),(2.22)},
   {i(16),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(17),i(False),i(-127),i(0),i(-32767),i(0),i(-2147483647),i(0),i(-9223372036854775807),(3.141592f),(3.141592653589793),(100.1)},
   {i(18),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(19),i(True),i(100),i(100),i(23000),i(23000),i(2047483647),i(2047483647),i(9123372036854775807),(1e-4f),(1e-4),(1.123)},
   {i(20),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(21),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(22),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(23),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
   {i(24),i(NULL_TINYINT),i(NULL_TINYINT),i(NULL_SMALLINT),i(NULL_SMALLINT),i(NULL_INT),i(NULL_INT),i(NULL_BIGINT),i(NULL_BIGINT),(NULL_FLOAT),(NULL_DOUBLE),(NULL_DOUBLE)},
  },
  result);
  // clang-format on
}

TEST_F(SelectQueryTest, ParquetNumericAndBooleanTypes) {
  const auto& query = getCreateForeignTableQuery(
      "( bool BOOLEAN, i8 TINYINT, u8 SMALLINT, i16 SMALLINT, "
      "u16 INT, i32 INT, u32 BIGINT, i64 BIGINT, f32 FLOAT, "
      "f64 DOUBLE, fixedpoint DECIMAL(10,5) )",
      "numeric_and_boolean_types",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table;");

  // clang-format off
  assertResultSetEqual({
   {i(True),i(100),i(100),i(23000),i(23000),i(2047483647),i(2047483647),i(9123372036854775807),(1e-4f),(1e-4),(1.123)},
   {i(False),i(-127),i(0),i(-32767),i(0),i(-2147483647),i(0),i(-9223372036854775807),(3.141592f),(3.141592653589793),(100.1)},
   {i(True),i(127),i(255),i(32767),i(65535),i(2147483647),i(4294967295),i(9223372036854775807),(1e9f),(1e19),(2.22)},
  },
  result);
  // clang-format on
}

TEST_F(SelectQueryTest, ParquetFixedEncodedTypes) {
  const auto& query = getCreateForeignTableQuery(
      "( i8 BIGINT ENCODING FIXED(8), u8 BIGINT ENCODING FIXED(16),"
      " i16 BIGINT ENCODING FIXED(16), "
      "u16 BIGINT ENCODING FIXED (32), i32 BIGINT ENCODING FIXED (32),"
      "i8_2 INT ENCODING FIXED(8), u8_2 INT ENCODING FIXED(16),"
      " i16_2 INT ENCODING FIXED(16),"
      "i8_3 SMALLINT ENCODING FIXED(8) )",
      "fixed_encoded_types",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table;");

  // clang-format off
  assertResultSetEqual({
      {i(100),i(100),i(23000),i(23000),i(2047483647),i(100),i(100),i(23000),i(100)},
      {i(-127),i(0),i(-32767),i(0),i(-2147483647),i(-127),i(0),i(-32767),i(-127)},
      {i(127),i(255),i(32767),i(65535),i(2147483647),i(127),i(255),i(32767),i(127)}
  },
  result);
  // clang-format on
}

TEST_F(SelectQueryTest, ParquetDecimalTypeMappings) {
  const auto& query = getCreateForeignTableQuery(
      "( decimal_i32 DECIMAL(8,3), decimal_i64 DECIMAL(10,3), decimal_fbla DECIMAL(7,3), "
      "decimal_ba DECIMAL(9,3)  ) ",
      "decimal",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table;");

  // clang-format off
  assertResultSetEqual({
   {1.123,1.123,1.123,1.123},
   {100.100,100.100,100.100,100.100},
   {2.220,2.220,2.220,2.220},
  },
  result);
  // clang-format on
}

TEST_F(SelectQueryTest, ParquetTimestampNoEncodingInSeconds) {
  const auto& query = getCreateForeignTableQuery(
      "(ts_milli TIMESTAMP, ts_micro TIMESTAMP, ts_nano TIMESTAMP)",
      "timestamp",
      "parquet");
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table;");

  assertResultSetEqual(
      {{NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
       {NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
       {NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
       {"1/1/1900 00:00:10", "1/1/1900 00:00:10", "1/1/1900 00:00:10"},
       {"1/1/2200 00:00:10", "1/1/2200 00:00:10", "1/1/2200 00:00:10"},
       {"8/25/2020 00:00:10", "8/25/2020 00:00:10", "8/25/2020 00:00:10"}},
      result);
}

TEST_F(SelectQueryTest, ParquetTimestampNoEncodingAllPrecisions) {
  const auto& query = getCreateForeignTableQuery(
      "(ts_milli TIMESTAMP (3), ts_micro TIMESTAMP (6), ts_nano TIMESTAMP (9))",
      "timestamp",
      "parquet");
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table;");
  assertResultSetEqual({{NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
                        {NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
                        {NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
                        {"1/1/1900 00:00:10.123",
                         "1/1/1900 00:00:10.123456",
                         "1/1/1900 00:00:10.123456789"},
                        {"1/1/2200 00:00:10.123",
                         "1/1/2200 00:00:10.123456",
                         "1/1/2200 00:00:10.123456789"},
                        {"8/25/2020 00:00:10.123",
                         "8/25/2020 00:00:10.123456",
                         "8/25/2020 00:00:10.123456789"}},
                       result);
}

TEST_F(SelectQueryTest, ParquetTimeNoEncodingInSeconds) {
  const auto& query = getCreateForeignTableQuery(
      "(time_milli TIME, time_micro TIME, time_nano TIME)", "time", "parquet");
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table;");
  assertResultSetEqual({{NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
                        {NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
                        {NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
                        {"00:00:01", "00:00:01", "00:00:01"},
                        {"00:00:00", "00:00:00", "00:00:00"},
                        {"23:59:59", "23:59:59", "23:59:59"}},
                       result);
}

TEST_F(SelectQueryTest, ParquetTimeFixedLength32EncodingInSeconds) {
  const auto& query = getCreateForeignTableQuery(
      "(time_milli TIME ENCODING FIXED(32), time_micro TIME, time_nano TIME)",
      "time",
      "parquet");
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table;");
  assertResultSetEqual({{NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
                        {NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
                        {NULL_BIGINT, NULL_BIGINT, NULL_BIGINT},
                        {"00:00:01", "00:00:01", "00:00:01"},
                        {"00:00:00", "00:00:00", "00:00:00"},
                        {"23:59:59", "23:59:59", "23:59:59"}},
                       result);
}

TEST_F(SelectQueryTest, ParquetDateNoEncoding) {
  const auto& query = getCreateForeignTableQuery("(days DATE)", "date", "parquet");
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table;");
  assertResultSetEqual({{NULL_BIGINT},
                        {NULL_BIGINT},
                        {NULL_BIGINT},
                        {"1/1/1900"},
                        {"1/1/2200"},
                        {"8/25/2020"}},
                       result);
}

TEST_F(SelectQueryTest, ParquetDateDays32Encoding) {
  const auto& query =
      getCreateForeignTableQuery("(days DATE ENCODING DAYS (32) )", "date", "parquet");
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * from test_foreign_table;");
  assertResultSetEqual({{NULL_BIGINT},
                        {NULL_BIGINT},
                        {NULL_BIGINT},
                        {"1/1/1900"},
                        {"1/1/2200"},
                        {"8/25/2020"}},
                       result);
}

TEST_F(SelectQueryTest, DirectoryWithDifferentSchema_SameNumberOfColumns) {
  std::string query = "CREATE FOREIGN TABLE test_foreign_table (t TIMESTAMP) "s +
                      "SERVER omnisci_local_parquet WITH (file_path = '" +
                      getDataFilesPath() + "/different_parquet_schemas_1');";
  sql(query);
  queryAndAssertException("SELECT * FROM test_foreign_table;",
                          "Exception: Parquet file \"" + getDataFilesPath() +
                              "different_parquet_schemas_1/timestamp_millis.parquet\" "
                              "has a different schema. Please ensure that all Parquet "
                              "files use the same schema. Reference Parquet file: " +
                              getDataFilesPath() +
                              "different_parquet_schemas_1/timestamp_micros.parquet, "
                              "column name: timestamp_micros. New Parquet file: " +
                              getDataFilesPath() +
                              "different_parquet_schemas_1/timestamp_millis.parquet, "
                              "column name: timestamp_millis.");
}

TEST_F(SelectQueryTest, DirectoryWithDifferentSchema_DifferentNumberOfColumns) {
  std::string query = "CREATE FOREIGN TABLE test_foreign_table (i INTEGER) "s +
                      "SERVER omnisci_local_parquet WITH (file_path = '" +
                      getDataFilesPath() + "/different_parquet_schemas_2');";
  sql(query);
  queryAndAssertException(
      "SELECT * FROM test_foreign_table;",
      "Exception: Parquet file \"" + getDataFilesPath() +
          "different_parquet_schemas_2/two_col_1_2.parquet\" has a different schema. "
          "Please ensure that all Parquet files use the same schema. Reference Parquet "
          "file: \"" +
          getDataFilesPath() +
          "different_parquet_schemas_2/1.parquet\" has 1 columns. New Parquet file \"" +
          getDataFilesPath() +
          "different_parquet_schemas_2/two_col_1_2.parquet\" has 2 columns.");
}

INSTANTIATE_TEST_SUITE_P(CachOnOffSelectQueryTests,
                         CacheControllingSelectQueryTest,
                         ::testing::Values(DiskCacheLevel::none, DiskCacheLevel::fsi),
                         PrintToStringParamName());

INSTANTIATE_TEST_SUITE_P(DataWrapperParameterizedTests,
                         DataWrapperSelectQueryTest,
                         ::testing::Values("csv", "parquet"),
                         PrintToStringParamName());

TEST_P(DataWrapperSelectQueryTest, AggregateAndGroupBy) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER, f DOUBLE)", "example_2", GetParam());
  sql(query);

  TQueryResult result;
  sql(result, "SELECT t, avg(i), sum(f) FROM test_foreign_table group by t;");
  // clang-format off
  assertResultSetEqual({{"a", 1.0, 1.1},
                        {"aa", 1.5, 3.3},
                        {"aaa", 2.0, 6.6}},
                       result);
  // clang-format on
}

// TODO: implement for parquet when kARRAY support implemented for parquet
TEST_P(CacheControllingSelectQueryTest, Join) {
  auto query = getCreateForeignTableQuery("(t TEXT, i INTEGER[])", "example_1", "csv");
  sql(query);

  query =
      getCreateForeignTableQuery("(t TEXT, i INTEGER, d DOUBLE)", "example_2", "csv", 2);
  sql(query);

  TQueryResult result;
  sql(result,
      "SELECT t1.t, t1.i, t2.i, t2.d FROM test_foreign_table AS t1 JOIN "
      "test_foreign_table_2 AS t2 ON t1.t = t2.t;");
  assertResultSetEqual({{"a", array({i(1), i(1), i(1)}), i(1), 1.1},
                        {"aa", array({Null_i, i(2), i(2)}), i(1), 1.1},
                        {"aa", array({Null_i, i(2), i(2)}), i(2), 2.2},
                        {"aaa", array({i(3), Null_i, i(3)}), i(1), 1.1},
                        {"aaa", array({i(3), Null_i, i(3)}), i(2), 2.2},
                        {"aaa", array({i(3), Null_i, i(3)}), i(3), 3.3}},
                       result);
}

TEST_P(DataWrapperSelectQueryTest, Filter) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER, f DOUBLE)", "example_2", GetParam());
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table WHERE i > 1;");
  // clang-format off
  assertResultSetEqual({{"aa", i(2), 2.2},
                        {"aaa", i(2), 2.2},
                        {"aaa", i(3), 3.3}},
                       result);
  // clang-format on
}

// TODO: implement for parquet when kARRAY support implemented for parquet
TEST_P(CacheControllingSelectQueryTest, Sort) {
  const auto& query =
      getCreateForeignTableQuery("(t TEXT, i INTEGER[])", "example_1", "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY t DESC;");
  assertResultSetEqual({{"aaa", array({i(3), Null_i, i(3)})},
                        {"aa", array({Null_i, i(2), i(2)})},
                        {"a", array({i(1), i(1), i(1)})}},
                       result);
}

TEST_P(DataWrapperSelectQueryTest, Update) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER, f DOUBLE)", "example_2", GetParam());
  sql(query);
  queryAndAssertException("UPDATE test_foreign_table SET t = 'abc';",
                          "Exception: DELETE, INSERT, OR UPDATE commands are not "
                          "supported for foreign tables.");
}

TEST_P(DataWrapperSelectQueryTest, Insert) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER, f DOUBLE)", "example_2", GetParam());
  sql(query);
  queryAndAssertException("INSERT INTO test_foreign_table VALUES('abc', null, null);",
                          "Exception: DELETE, INSERT, OR UPDATE commands are not "
                          "supported for foreign tables.");
}

TEST_P(DataWrapperSelectQueryTest, InsertIntoSelect) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER, f DOUBLE)", "example_2", GetParam());
  sql(query);
  queryAndAssertException(
      "INSERT INTO test_foreign_table SELECT * FROM test_foreign_table;",
      "Exception: DELETE, INSERT, OR UPDATE commands are not supported for "
      "foreign "
      "tables.");
}

TEST_P(DataWrapperSelectQueryTest, Delete) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER, f DOUBLE)", "example_2", GetParam());
  sql(query);
  queryAndAssertException("DELETE FROM test_foreign_table WHERE t = 'a';",
                          "Exception: DELETE, INSERT, OR UPDATE commands are not "
                          "supported for foreign tables.");
}

TEST_P(CacheControllingSelectQueryTest, CSV_CustomDelimiters) {
  const auto& query = getCreateForeignTableQuery(
      "(b BOOLEAN, i INTEGER, f FLOAT, t TIME, tp TIMESTAMP, d DATE, "
      "txt TEXT, txt_2 TEXT, i_arr INTEGER[], txt_arr TEXT[])",
      {{"delimiter", "|"}, {"array_delimiter", "_"}},
      "custom_delimiters",
      "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  // clang-format off
  assertResultSetEqual({
    {
      True, i(30000), 10.1f, "00:00:10", "1/1/2000 00:00:59", "1/1/2000", "text_1",
      "quoted text", array({i(1)}), array({"quoted text"})
    },
    {
      False, i(30500), 100.12f, "00:10:00", "6/15/2020 00:59:59", "6/15/2020", "text_2",
      "quoted text 2", array({i(1), i(2), i(3)}), array({"quoted text 2", "quoted text 3"})
    },
    {
      True, i(31000), 1000.123f, "10:00:00", "12/31/2500 23:59:59", "12/31/2500", "text_3",
      "quoted text 3", array({i(10), i(20), i(30)}), array({"quoted_text_4", "quoted_text_5"})
    }},
    result);
  // clang-format on
}

class CSVFileTypeTests
    : public SelectQueryTest,
      public ::testing::WithParamInterface<std::pair<std::string, std::string>> {};

INSTANTIATE_TEST_SUITE_P(
    CSVFileTypeParameterizedTests,
    CSVFileTypeTests,
    ::testing::Values(std::make_pair("example_1.csv", "uncompressed"),
                      std::make_pair("example_1.zip", "zip"),
                      std::make_pair("example_1_newline.zip", "zip_newline"),
                      std::make_pair("example_1_multi.zip", "multi_zip"),
                      std::make_pair("example_1_multilevel.zip", "multilevel_zip"),
                      std::make_pair("example_1.tar.gz", "tar_gz"),
                      std::make_pair("example_1_multi.tar.gz", "multi_tar_gz"),
                      std::make_pair("example_1.7z", "7z"),
                      std::make_pair("example_1.rar", "rar"),
                      std::make_pair("example_1.bz2", "bz2"),
                      std::make_pair("example_1_multi.7z", "7z_multi"),
                      std::make_pair("example_1.csv.gz", "gz"),
                      std::make_pair("example_1_dir", "dir"),
                      std::make_pair("example_1_dir_newline", "dir_newline"),
                      std::make_pair("example_1_dir_archives", "dir_archives"),
                      std::make_pair("example_1_dir_multilevel", "multilevel_dir")),
    PrintToStringParamName());

TEST_P(CSVFileTypeTests, SelectCSV) {
  std::string query = "CREATE FOREIGN TABLE test_foreign_table (t TEXT, i INTEGER[]) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "/" + GetParam().first + "');";
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table  ORDER BY t;");
  assertResultSetEqual({{"a", array({i(1), i(1), i(1)})},
                        {"aa", array({Null_i, i(2), i(2)})},
                        {"aaa", array({i(3), Null_i, i(3)})}},
                       result);
}

TEST_P(CacheControllingSelectQueryTest, CsvEmptyArchive) {
  std::string query = "CREATE FOREIGN TABLE test_foreign_table (t TEXT, i INTEGER[]) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "/" + "example_1_empty.zip" + "');";
  sql(query);
  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table  ORDER BY t;");
  assertResultSetEqual({}, result);
}

TEST_P(CacheControllingSelectQueryTest, CsvDirectoryBadFileExt) {
  std::string query = "CREATE FOREIGN TABLE test_foreign_table (t TEXT, i INTEGER[]) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "/" + "example_1_dir_bad_ext/" + "');";
  sql(query);
  queryAndAssertException("SELECT * FROM test_foreign_table  ORDER BY t;",
                          "Exception: Invalid extention for file \"" +
                              getDataFilesPath() +
                              "example_1_dir_bad_ext/example_1c.tmp\".");
}

TEST_P(CacheControllingSelectQueryTest, CsvArchiveInvalidFile) {
  std::string query = "CREATE FOREIGN TABLE test_foreign_table (t TEXT, i INTEGER[]) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "/" + "example_1_invalid_file.zip" + "');";
  sql(query);
  queryAndAssertException("SELECT * FROM test_foreign_table  ORDER BY t;",
                          "Exception: Mismatched number of logical columns: (expected 2 "
                          "columns, has 1): (random text)");
}

TEST_P(CacheControllingSelectQueryTest, CSV_CustomLineDelimiters) {
  const auto& query = getCreateForeignTableQuery("(b BOOLEAN, i INTEGER, t TEXT)",
                                                 {{"line_delimiter", "*"}},
                                                 "custom_line_delimiter",
                                                 "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  // clang-format off
  assertResultSetEqual({{True, i(1000), "text_1"},
                        {False, i(2000), "text_2"},
                        {True, i(3000), "text_3"}},
                       result);
  // clang-format on
}

// TODO: implement for parquet when kARRAY support implemented for parquet
// Note: only need to test array_marker and array_delimiter
TEST_P(CacheControllingSelectQueryTest, CSV_CustomMarkers) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, t2 TEXT, i INTEGER[])",
      {{"array_marker", "[]"}, {"escape", "\\"}, {"nulls", "NIL"}, {"quote", "|"}},
      "custom_markers",
      "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{Null, "quoted |text|", array({i(1)})},
                        {"text_1", "quoted text", array({i(1), i(2)})},
                        {Null, "\"quoted\" \"text\"", array({i(3), i(4), i(5)})}},
                       result);
}

TEST_P(CacheControllingSelectQueryTest, CSV_NoHeader) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER[])", {{"header", "false"}}, "no_header", "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{"a", array({i(1), i(1), i(1)})},
                        {"aa", array({i(2), i(2), i(2)})},
                        {"aaa", array({i(3), i(3), i(3)})}},
                       result);
}

TEST_P(CacheControllingSelectQueryTest, CSV_QuotedHeader) {
  const auto& query =
      getCreateForeignTableQuery("(t TEXT, i INTEGER[])", "quoted_headers", "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{"a", array({i(1), i(1), i(1)})},
                        {"aa", array({i(2), i(2), i(2)})},
                        {"aaa", array({i(3), i(3), i(3)})}},
                       result);
}

TEST_P(CacheControllingSelectQueryTest, CSV_NonQuotedFields) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, t2 TEXT)", {{"quoted", "false"}}, "non_quoted", "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  // clang-format off
  assertResultSetEqual({{"text_1", "\"text_1\""},
                        {"text_2", "\"text_2\""},
                        {"text_3", "\"text_3\""}},
                       result);
  // clang-format on
}

TEST_P(CacheControllingSelectQueryTest, WithBufferSizeOption) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER[])", {{"buffer_size", "25"}}, "example_1", "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY t;");
  assertResultSetEqual({{"a", array({i(1), i(1), i(1)})},
                        {"aa", array({Null_i, i(2), i(2)})},
                        {"aaa", array({i(3), Null_i, i(3)})}},
                       result);
}

TEST_P(CacheControllingSelectQueryTest, WithBufferSizeLessThanRowSize) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER[])", {{"buffer_size", "10"}}, "example_1", "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY t;");
  assertResultSetEqual({{"a", array({i(1), i(1), i(1)})},
                        {"aa", array({Null_i, i(2), i(2)})},
                        {"aaa", array({i(3), Null_i, i(3)})}},
                       result);
}

TEST_P(CacheControllingSelectQueryTest, WithMaxBufferResizeLessThanRowSize) {
  import_export::delimited_parser::set_max_buffer_resize(15);
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER[])", {{"buffer_size", "10"}}, "example_1", "csv");
  sql(query);

  queryAndAssertException(
      "SELECT * FROM test_foreign_table ORDER BY t;",
      "Exception: Unable to find an end of line character after reading 14 characters. "
      "Please ensure that the correct \"line_delimiter\" option is specified or update "
      "the \"buffer_size\" option appropriately. Row number: 2. "
      "First few characters in row: aa,{'NA', 2, 2");
}

TEST_P(CacheControllingSelectQueryTest, ReverseLongitudeAndLatitude) {
  const auto& query = getCreateForeignTableQuery(
      "(p POINT)", {{"lonlat", "false"}}, "reversed_long_lat", "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  // clang-format off
  assertResultSetEqual({{"POINT (1 0)"},
                        {"POINT (2 1)"},
                        {"POINT (3 2)"}},
                       result);
  // clang-format on
}

TEST_F(SelectQueryTest, UnsupportedColumnMapping) {
  const auto& query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER, f INTEGER)", {}, "example_2", "parquet");
  sql(query);
  queryAndAssertException(
      "SELECT * FROM test_foreign_table;",
      "Exception: Conversion from Parquet type \"DOUBLE\" to OmniSci type \"INTEGER\" is "
      "not allowed. Please use an appropriate column type.");
}

TEST_F(SelectQueryTest, NoStatistics) {
  const auto& query = getCreateForeignTableQuery(
      "(a BIGINT, b BIGINT, c TEXT, d DOUBLE)", {}, "no_stats", "parquet");
  sql(query);
  queryAndAssertException(
      "SELECT * FROM test_foreign_table;",
      "Exception: Statistics metadata is required for all row groups. Metadata is "
      "missing for row group index: 0, column index: 0, file path: " +
          getDataFilesPath() + "no_stats.parquet");
}

TEST_F(SelectQueryTest, RowGroupSizeLargerThanFragmentSize) {
  const auto& query =
      getCreateForeignTableQuery("(a INTEGER, b INTEGER, c INTEGER, d DOUBLE)",
                                 {{"fragment_size", "1"}},
                                 "row_group_size_2",
                                 "parquet");
  sql(query);
  queryAndAssertException(
      "SELECT * FROM test_foreign_table;",
      "Exception: Parquet file has a row group size that is larger than the fragment "
      "size. Please set the table fragment size to a number that is larger than the row "
      "group size. Row group index: 0, row group size: 2, fragment size: 1, file path: " +
          getDataFilesPath() + "row_group_size_2.parquet");
}

TEST_F(SelectQueryTest, NonUtcTimestamp) {
  const auto& query = getCreateForeignTableQuery(
      "(tstamp TIMESTAMP)", {}, "non_utc_timestamp", "parquet");
  sql(query);
  queryAndAssertException("SELECT * FROM test_foreign_table;",
                          "Exception: Non-UTC timezone specified in Parquet file for "
                          "column \"tstamp\". Only UTC timezone is currently supported.");
}

TEST_F(SelectQueryTest, DecimalIntEncoding) {
  const auto& query = getCreateForeignTableQuery(
      "(decimal_int_32 DECIMAL(9, 5), decimal_int_64 DECIMAL(15, 10))",
      {},
      "decimal_int_encoding",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{100.1234, 100.1234}, {2.1234, 2.1234}, {100.1, 100.1}}, result);
}

TEST_F(SelectQueryTest, ByteArrayDecimalFilterAndSort) {
  const auto& query = getCreateForeignTableQuery(
      "(dc DECIMAL(4, 2))", {{"fragment_size", "3"}}, "byte_array_decimal", "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table where dc > 25 ORDER BY dc;");
  assertResultSetEqual({{25.55}, {50.11}}, result);
}

class RefreshForeignTableTest : public ForeignTableTest {
 protected:
  std::string table_1_filename = "refresh_tmp_1";
  std::string table_2_filename = "refresh_tmp_2";
  std::string table_1_name = default_table_name;
  std::string table_2_name = default_table_name + "_1";
  Catalog_Namespace::Catalog* cat;
  foreign_storage::ForeignStorageCache* cache;
  ChunkKey key_1, key_2;

  void SetUp() override {
    ForeignTableTest::SetUp();
    cat = &getCatalog();
    cache = cat->getDataMgr().getPersistentStorageMgr()->getDiskCache();
    cache->clear();
  }

  void TearDown() override {
    bf::remove(getDataFilesPath() + table_1_filename + ".csv");
    bf::remove(getDataFilesPath() + table_2_filename + ".csv");
    sqlDropForeignTable(0, table_1_name);
    sqlDropForeignTable(0, table_2_name);
    ForeignTableTest::TearDown();
  }

  bool isChunkAndMetadataCached(const ChunkKey& chunk_key) {
    if (cache->getCachedChunkIfExists(chunk_key) != nullptr &&
        cache->isMetadataCached(chunk_key)) {
      return true;
    }
    return false;
  }
};

class RefreshTests : public ForeignTableTest {
 protected:
  const std::string default_name = "refresh_tmp";
  std::string file_type;
  std::vector<std::string> tmp_file_names;
  std::vector<std::string> table_names;
  Catalog_Namespace::Catalog* cat;
  foreign_storage::ForeignStorageCache* cache;

  void SetUp() override {
    ForeignTableTest::SetUp();
    cat = &getCatalog();
    cache = cat->getDataMgr().getPersistentStorageMgr()->getDiskCache();
    cache->clear();
    sql("DROP FOREIGN TABLE IF EXISTS " + default_name + ";");
  }

  void TearDown() override {
    for (auto file_name : tmp_file_names) {
      bf::remove(getDataFilesPath() + file_name + "." + file_type);
    }
    for (auto table_name : table_names) {
      sqlDropForeignTable(0, table_name);
    }
    sqlDropForeignTable(0, default_name);
    ForeignTableTest::TearDown();
  }

  bool isChunkAndMetadataCached(const ChunkKey& chunk_key) {
    if (cache->getCachedChunkIfExists(chunk_key) != nullptr &&
        cache->isMetadataCached(chunk_key)) {
      return true;
    }
    return false;
  }

  void createFilesAndTables(
      const std::vector<std::string>& file_names,
      const std::string& column_schema = "(i INTEGER)",
      const std::map<std::string, std::string>& table_options = {}) {
    for (size_t i = 0; i < file_names.size(); ++i) {
      tmp_file_names.emplace_back(default_name + std::to_string(i));
      table_names.emplace_back(default_name + std::to_string(i));
      bf::copy_file(getDataFilesPath() + file_names[i] + "." + file_type,
                    getDataFilesPath() + tmp_file_names[i] + "." + file_type,
                    bf::copy_option::overwrite_if_exists);
      sqlCreateForeignTable(
          column_schema, tmp_file_names[i], file_type, table_options, 0, table_names[i]);
    }
  }

  int64_t getCurrentTime() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  std::pair<int64_t, int64_t> getLastAndNextRefreshTimes(
      const std::string& table_name = "test_foreign_table") {
    auto table = getCatalog().getMetadataForTable(table_name, false);
    CHECK(table);
    const auto foreign_table = dynamic_cast<const foreign_storage::ForeignTable*>(table);
    CHECK(foreign_table);
    return {foreign_table->last_refresh_time, foreign_table->next_refresh_time};
  }

  void assertNullRefreshTime(int64_t refresh_time) { ASSERT_EQ(-1, refresh_time); }

  void assertRefreshTimeBetween(int64_t refresh_time,
                                int64_t start_time,
                                int64_t end_time) {
    ASSERT_GE(refresh_time, start_time);
    ASSERT_LE(refresh_time, end_time);
  }
};

TEST_F(RefreshTests, InvalidRefreshMode) {
  std::string filename = "archive_delete_file.zip";
  std::string query = "CREATE FOREIGN TABLE " + default_name + " (i INTEGER) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "append_before/" + filename +
                      "', fragment_size = '1' " + ", REFRESH_UPDATE_TYPE = 'INVALID');";
  queryAndAssertException(
      query,
      "Exception: Invalid value \"INVALID\" for REFRESH_UPDATE_TYPE option. "
      "Value must be \"APPEND\" or \"ALL\".");
}

void recursive_copy(const std::string& origin, const std::string& dest) {
  bf::create_directory(dest);
  for (bf::directory_iterator file(origin); file != bf::directory_iterator(); ++file) {
    const auto& path = file->path();
    if (bf::is_directory(path)) {
      recursive_copy(path.string(), dest + "/" + path.filename().string());
    } else {
      bf::copy_file(path.string(), dest + "/" + path.filename().string());
    }
  }
}

bool does_cache_contain_chunks(Catalog_Namespace::Catalog* cat,
                               const std::string& table_name,
                               const std::vector<std::vector<int>> subkeys) {
  // subkey is chunkey without db, table ids
  auto td = cat->getMetadataForTable(table_name);
  ChunkKey table_key{cat->getCurrentDB().dbId, td->tableId};
  auto cache = cat->getDataMgr().getPersistentStorageMgr()->getDiskCache();

  for (const auto& subkey : subkeys) {
    auto chunk_key = table_key;
    chunk_key.insert(chunk_key.end(), subkey.begin(), subkey.end());
    if (cache->getCachedChunkIfExists(chunk_key) == nullptr) {
      return false;
    }
  }
  return true;
}

// Check that chunks/metadata are fully cleared in bulk update mode
TEST_F(RefreshTests, BulkUpdateCacheUpdate) {
  int fragment_size = 1;
  std::string filename = "single_file.csv";
  auto cache = getCatalog().getDataMgr().getPersistentStorageMgr()->getDiskCache();
  // Create initial files and tables
  bf::remove_all(getDataFilesPath() + "append_tmp");

  recursive_copy(getDataFilesPath() + "append_before", getDataFilesPath() + "append_tmp");
  std::string file_path = getDataFilesPath() + "append_tmp/" + "single_file.csv";

  std::string query = "CREATE FOREIGN TABLE " + default_name + " (i INTEGER) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "append_tmp/" + filename +
                      "', fragment_size = '" + std::to_string(fragment_size) +
                      "', REFRESH_UPDATE_TYPE = 'ALL');";
  sql(query);

  std::string select = "SELECT * FROM "s + default_name + " ORDER BY i;";
  // Read from table
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});
  bf::remove_all(getDataFilesPath() + "append_tmp");
  recursive_copy(getDataFilesPath() + "append_after", getDataFilesPath() + "append_tmp");
  size_t mdata_count = cache->getNumMetadataAdded();
  size_t chunk_count = cache->getNumChunksAdded();

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + default_name + ";");

  // All  chunks + will be updated
  size_t update_count = 5;
  ASSERT_EQ(update_count, cache->getNumMetadataAdded() - mdata_count);
  // 2 original chunks are recached
  ASSERT_EQ(2U, cache->getNumChunksAdded() - chunk_count);

  // cache contains original chunks;
  ASSERT_TRUE(does_cache_contain_chunks(cat, default_name, {{1, 0}, {1, 1}}));

  sqlAndCompareResult(select, {{i(1)}, {i(2)}, {i(3)}, {i(4)}, {i(5)}});
  ASSERT_EQ(update_count, cache->getNumMetadataAdded() - mdata_count);
  // Now all new chunks are cached
  ASSERT_EQ(update_count, cache->getNumChunksAdded() - chunk_count);
  ASSERT_TRUE(does_cache_contain_chunks(
      cat, default_name, {{1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4}}));

  bf::remove_all(getDataFilesPath() + "append_tmp");
}

class RefreshMetadataTypeTest : public SelectQueryTest {};
TEST_F(RefreshMetadataTypeTest, ScalarTypes) {
  const auto& query = getCreateForeignTableQuery(
      "(b BOOLEAN, t TINYINT, s SMALLINT, i INTEGER, bi BIGINT, f FLOAT, "
      "dc DECIMAL(10, 5), tm TIME, tp TIMESTAMP, d DATE, txt TEXT, "
      "txt_2 TEXT ENCODING NONE)",
      {},
      "scalar_types",
      "csv",
      0,
      default_table_name,
      "csv");
  sql(query);
  sql("SELECT * FROM " + default_table_name + ";");
  sql("REFRESH FOREIGN TABLES " + default_table_name + ";");
  sql("SELECT * FROM " + default_table_name + ";");
}

TEST_F(RefreshMetadataTypeTest, ArrayTypes) {
  const auto& query = getCreateForeignTableQuery(
      "(index int, b BOOLEAN[], t TINYINT[], s SMALLINT[], i INTEGER[], bi BIGINT[], f "
      "FLOAT[], "
      "tm "
      "TIME[], tp TIMESTAMP[], "
      "d DATE[], txt TEXT[], fixedpoint DECIMAL(10,5)[])",
      {},
      "array_types",
      "csv",
      0,
      default_table_name,
      "csv");
  sql(query);
  sql("SELECT * FROM " + default_table_name + ";");
  sql("REFRESH FOREIGN TABLES " + default_table_name + ";");
  sql("SELECT * FROM " + default_table_name + ";");
}

TEST_F(RefreshMetadataTypeTest, GeoTypes) {
  const auto& query = getCreateForeignTableQuery(
      "(index int, p POINT, l LINESTRING, poly POLYGON, multipoly MULTIPOLYGON)",
      {},
      "geo_types",
      "csv",
      0,
      default_table_name,
      "csv");
  sql(query);
  sql("SELECT * FROM " + default_table_name + ";");
  sql("REFRESH FOREIGN TABLES " + default_table_name + ";");
  sql("SELECT * FROM " + default_table_name + ";");
}

class RefreshParamTests : public RefreshTests,
                          public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {
    file_type = GetParam();
    RefreshTests::SetUp();
  }
};

INSTANTIATE_TEST_SUITE_P(RefreshParamTestsParameterizedTests,
                         RefreshParamTests,
                         ::testing::Values("csv", "parquet"),
                         PrintToStringParamName());

TEST_P(RefreshParamTests, SingleTable) {
  // Create initial files and tables
  createFilesAndTables({"0"});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ChunkKey orig_key = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "1" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ";");

  // Compare new results
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}});
}

TEST_P(RefreshParamTests, FragmentSkip) {
  // Create initial files and tables
  createFilesAndTables({"0", "1"});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + " WHERE i >= 3;", {});
  ChunkKey orig_key0 = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key0), nullptr);
  ASSERT_TRUE(cache->isMetadataCached(orig_key0));

  sqlAndCompareResult("SELECT * FROM " + table_names[1] + " WHERE i >= 3;", {});
  ChunkKey orig_key1 = getChunkKeyFromTable(*cat, table_names[1], {1, 0});
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key1), nullptr);
  ASSERT_TRUE(cache->isMetadataCached(orig_key1));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "2" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);
  bf::copy_file(getDataFilesPath() + "3" + "." + file_type,
                getDataFilesPath() + tmp_file_names[1] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + " WHERE i >= 3;", {});
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key0), nullptr);
  ASSERT_TRUE(cache->isMetadataCached(orig_key0));

  sqlAndCompareResult("SELECT * FROM " + table_names[1] + " WHERE i >= 3;", {});
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key1), nullptr);
  ASSERT_TRUE(cache->isMetadataCached(orig_key1));

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ", " + tmp_file_names[1] + ";");

  // Compare new results
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key0), nullptr);
  ASSERT_TRUE(cache->isMetadataCached(orig_key0));
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key1), nullptr);
  ASSERT_TRUE(cache->isMetadataCached(orig_key1));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + " WHERE i >= 3;", {});
  sqlAndCompareResult("SELECT * FROM " + table_names[1] + " WHERE i >= 3;", {{i(3)}});
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key0), nullptr);
  ASSERT_TRUE(cache->isMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));
}

TEST_P(RefreshParamTests, TwoTable) {
  // Create initial files and tables
  createFilesAndTables({"0", "1"});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ChunkKey orig_key0 = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));

  sqlAndCompareResult("SELECT * FROM " + table_names[1] + ";", {{i(1)}});
  ChunkKey orig_key1 = getChunkKeyFromTable(*cat, table_names[1], {1, 0});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "2" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);
  bf::copy_file(getDataFilesPath() + "3" + "." + file_type,
                getDataFilesPath() + tmp_file_names[1] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));

  sqlAndCompareResult("SELECT * FROM " + table_names[1] + ";", {{i(1)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ", " + tmp_file_names[1] + ";");

  // Compare new results
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(2)}});
  sqlAndCompareResult("SELECT * FROM " + table_names[1] + ";", {{i(3)}});
}

TEST_P(RefreshParamTests, EvictTrue) {
  // Create initial files and tables
  createFilesAndTables({"0"});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ChunkKey orig_key = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "1" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Refresh command
  auto start_time = getCurrentTime();
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + " WITH (evict = true);");
  auto end_time = getCurrentTime();

  // Compare new results
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key), nullptr);
  ASSERT_FALSE(cache->isMetadataCached(orig_key));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}});

  auto [last_refresh_time, next_refresh_time] =
      getLastAndNextRefreshTimes(table_names[0]);
  assertRefreshTimeBetween(last_refresh_time, start_time, end_time);
  assertNullRefreshTime(next_refresh_time);
}

TEST_P(RefreshParamTests, TwoColumn) {
  // Create initial files and tables
  createFilesAndTables({"two_col_1_2"}, "(i INTEGER, i2 INTEGER)");

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1), i(2)}});
  ChunkKey orig_key0 = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ChunkKey orig_key1 = getChunkKeyFromTable(*cat, table_names[0], {2, 0});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "two_col_3_4" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1), i(2)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ";");

  // Compare new results
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(3), i(4)}});
}

TEST_P(RefreshParamTests, ChangeSchema) {
  // Create initial files and tables
  createFilesAndTables({"1"});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}});
  ChunkKey orig_key = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "two_col_3_4" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Refresh command
  try {
    sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ";");
    FAIL() << "An exception should have been thrown";
  } catch (const std::exception& e) {
    ASSERT_NE(strstr(e.what(), "Mismatched number of logical columns"), nullptr);
  }
}

TEST_P(RefreshParamTests, AddFrags) {
  // Create initial files and tables
  createFilesAndTables({"two_row_1_2"}, "(i INTEGER)", {{"fragment_size", "1"}});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}, {i(2)}});
  ChunkKey orig_key0 = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ChunkKey orig_key1 = getChunkKeyFromTable(*cat, table_names[0], {1, 1});
  ChunkKey orig_key2 = getChunkKeyFromTable(*cat, table_names[0], {1, 2});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "three_row_3_4_5" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}, {i(2)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ";");

  // Compare new results
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key2), nullptr);
  ASSERT_TRUE(cache->isMetadataCached(orig_key2));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(3)}, {i(4)}, {i(5)}});
}

TEST_P(RefreshParamTests, SubFrags) {
  // Create initial files and tables
  createFilesAndTables({"three_row_3_4_5"}, "(i INTEGER)", {{"fragment_size", "1"}});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(3)}, {i(4)}, {i(5)}});
  ChunkKey orig_key0 = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ChunkKey orig_key1 = getChunkKeyFromTable(*cat, table_names[0], {1, 1});
  ChunkKey orig_key2 = getChunkKeyFromTable(*cat, table_names[0], {1, 2});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key2));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "two_row_1_2" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(3)}, {i(4)}, {i(5)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key2));

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ";");

  // Compare new results
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));
  ASSERT_EQ(cache->getCachedChunkIfExists(orig_key2), nullptr);
  ASSERT_FALSE(cache->isMetadataCached(orig_key2));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}, {i(2)}});
}

TEST_P(RefreshParamTests, TwoFrags) {
  // Create initial files and tables
  createFilesAndTables({"two_row_1_2"}, "(i INTEGER)", {{"fragment_size", "1"}});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}, {i(2)}});
  ChunkKey orig_key0 = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ChunkKey orig_key1 = getChunkKeyFromTable(*cat, table_names[0], {1, 1});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "two_row_3_4" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}, {i(2)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ";");

  // Compare new results
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key0));
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key1));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(3)}, {i(4)}});
}

TEST_P(RefreshParamTests, String) {
  // Create initial files and tables
  createFilesAndTables({"a"}, "(t TEXT)");

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{"a"}});
  ChunkKey orig_key = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "b" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{"a"}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ";");

  // Compare new results
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{"b"}});
}

class RefreshDeviceTests : public RefreshTests,
                           public ::testing::WithParamInterface<TExecuteMode::type> {
 protected:
  void SetUp() override {
    RefreshTests::SetUp();
    file_type = "csv";
  }
};
INSTANTIATE_TEST_SUITE_P(RefreshDeviceTestsParameterizedTests,
                         RefreshDeviceTests,
                         ::testing::Values(TExecuteMode::CPU, TExecuteMode::GPU),
                         PrintToStringParamName());

TEST_P(RefreshDeviceTests, Device) {
  if (!setExecuteMode(GetParam())) {
    return;
  }
  // Create initial files and tables
  createFilesAndTables({"0"});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ChunkKey orig_key = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "1" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + ";");

  // Compare new results
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}});
}

class RefreshSyntaxTests : public RefreshTests,
                           public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {
    RefreshTests::SetUp();
    file_type = "csv";
  }
};
INSTANTIATE_TEST_SUITE_P(RefreshSyntaxTestsParameterizedTests,
                         RefreshSyntaxTests,
                         ::testing::Values(" WITH (evict = false)",
                                           " WITH (EVICT = FALSE)"));

TEST_P(RefreshSyntaxTests, EvictFalse) {
  // Create initial files and tables
  createFilesAndTables({"0"});

  // Read from table to populate cache.
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ChunkKey orig_key = getChunkKeyFromTable(*cat, table_names[0], {1, 0});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Change underlying file
  bf::copy_file(getDataFilesPath() + "1" + "." + file_type,
                getDataFilesPath() + tmp_file_names[0] + "." + file_type,
                bf::copy_option::overwrite_if_exists);

  // Confirm chaning file hasn't changed cached results
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(0)}});
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));

  // Refresh command
  auto start_time = getCurrentTime();
  sql("REFRESH FOREIGN TABLES " + tmp_file_names[0] + GetParam() + ";");
  auto end_time = getCurrentTime();

  // Compare new results
  ASSERT_TRUE(isChunkAndMetadataCached(orig_key));
  sqlAndCompareResult("SELECT * FROM " + table_names[0] + ";", {{i(1)}});

  auto [last_refresh_time, next_refresh_time] =
      getLastAndNextRefreshTimes(table_names[0]);
  assertRefreshTimeBetween(last_refresh_time, start_time, end_time);
  assertNullRefreshTime(next_refresh_time);
}

class RefreshSyntaxErrorTests : public RefreshTests {
 protected:
  void SetUp() override {
    RefreshTests::SetUp();
    file_type = "csv";
  }
};

TEST_F(RefreshSyntaxErrorTests, InvalidEvictValue) {
  createFilesAndTables({"0"});
  std::string query{"REFRESH FOREIGN TABLES " + tmp_file_names[0] +
                    " WITH (evict = 'invalid');"};
  queryAndAssertException(query,
                          "Exception: Invalid value \"invalid\" provided for EVICT "
                          "option. Value must be either \"true\" or \"false\".");
}

TEST_F(RefreshSyntaxErrorTests, InvalidOption) {
  createFilesAndTables({"0"});
  std::string query{"REFRESH FOREIGN TABLES " + tmp_file_names[0] +
                    " WITH (invalid_key = false);"};
  queryAndAssertException(query,
                          "Exception: Invalid option \"INVALID_KEY\" provided for "
                          "refresh command. Only \"EVICT\" option is supported.");
}

class AppendRefreshTest : public RecoverCacheQueryTest,
                          public ::testing::WithParamInterface<AppendRefreshTestParam> {
 protected:
  const std::string default_name = "refresh_tmp";
  std::string file_type;

  void SetUp() override {
    RecoverCacheQueryTest::SetUp();
    sqlDropForeignTable(0, default_name);
  }

  void TearDown() override {
    sqlDropForeignTable(0, default_name);
    RecoverCacheQueryTest::TearDown();
  }
};

TEST_F(AppendRefreshTest, CSV_MissingFileArchive) {
  int fragment_size = 1;
  std::string filename = "archive_delete_file.zip";
  // Create initial files and tables
  bf::remove_all(getDataFilesPath() + "append_tmp");
  recursive_copy(getDataFilesPath() + "append_before", getDataFilesPath() + "append_tmp");

  std::string query = "CREATE FOREIGN TABLE " + default_name + " (i INTEGER) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "append_tmp/" + filename +
                      "', fragment_size = '" + std::to_string(fragment_size) +
                      "', REFRESH_UPDATE_TYPE = 'APPEND');";
  sql(query);

  std::string select = "SELECT * FROM "s + default_name + " ORDER BY i;";
  // Read from table
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});

  // Modify files
  bf::remove_all(getDataFilesPath() + "append_tmp");
  recursive_copy(getDataFilesPath() + "append_after", getDataFilesPath() + "append_tmp");

  // Refresh command
  queryAndAssertException(
      "REFRESH FOREIGN TABLES " + default_name + ";",
      "Exception: Foreign table refreshed with APPEND mode missing archive entry "
      "\"single_file_delete_rows.csv\" from file \"archive_delete_file.zip\".");

  bf::remove_all(getDataFilesPath() + "append_tmp");
}

class FragmentSizesAppendRefreshTest : public AppendRefreshTest {};

INSTANTIATE_TEST_SUITE_P(
    AppendParamaterizedTests,
    FragmentSizesAppendRefreshTest,
    ::testing::Values(
        AppendRefreshTestParam{1, "csv", "single_file.csv", "single_csv", false},
        AppendRefreshTestParam{1,
                               "parquet",
                               "single_file.parquet",
                               "single_parquet",
                               false},
        AppendRefreshTestParam{1, "csv", "csv_dir_file", "csv_dir", false},
        AppendRefreshTestParam{1, "parquet", "parquet_dir_file", "parquet_dir", false},
        AppendRefreshTestParam{1, "csv", "dir_file.zip", "csv_dir_zip", false},
        AppendRefreshTestParam{1,
                               "csv",
                               "csv_dir_file_multi",
                               "csv_dir_file_multi",
                               false},
        AppendRefreshTestParam{1,
                               "parquet",
                               "parquet_dir_file_multi",
                               "parquet_dir_file_multi",
                               false},
        AppendRefreshTestParam{1,
                               "csv",
                               "dir_file_multi.zip",
                               "csv_dir_multi_zip",
                               false},
        AppendRefreshTestParam{1, "csv", "single_file.zip", "csv_single_zip", false},
        AppendRefreshTestParam{4, "csv", "single_file.csv", "single_csv", false},
        AppendRefreshTestParam{4,
                               "parquet",
                               "single_file.parquet",
                               "single_parquet",
                               false},
        AppendRefreshTestParam{4, "csv", "csv_dir_file", "csv_dir", false},
        AppendRefreshTestParam{4, "parquet", "parquet_dir_file", "parquet_dir", false},
        AppendRefreshTestParam{4, "csv", "dir_file.zip", "csv_dir_zip", false},
        AppendRefreshTestParam{4,
                               "csv",
                               "csv_dir_file_multi",
                               "csv_dir_file_multi",
                               false},
        AppendRefreshTestParam{4,
                               "parquet",
                               "parquet_dir_file_multi",
                               "parquet_dir_file_multi",
                               false},
        AppendRefreshTestParam{4,
                               "csv",
                               "dir_file_multi.zip",
                               "csv_dir_multi_zip",
                               false},
        AppendRefreshTestParam{4, "csv", "single_file.zip", "csv_single_zip", false},
        AppendRefreshTestParam{32000000, "csv", "single_file.csv", "single_csv", false},
        AppendRefreshTestParam{32000000,
                               "parquet",
                               "single_file.parquet",
                               "single_parquet",
                               false},
        AppendRefreshTestParam{32000000, "csv", "csv_dir_file", "csv_dir", false},
        AppendRefreshTestParam{32000000,
                               "parquet",
                               "parquet_dir_file",
                               "parquet_dir",
                               false},
        AppendRefreshTestParam{32000000, "csv", "dir_file.zip", "csv_dir_zip"},
        AppendRefreshTestParam{32000000,
                               "csv",
                               "csv_dir_file_multi",
                               "csv_dir_file_multi",
                               false},
        AppendRefreshTestParam{32000000,
                               "parquet",
                               "parquet_dir_file_multi",
                               "parquet_dir_file_multi",
                               false},
        AppendRefreshTestParam{32000000,
                               "csv",
                               "dir_file_multi.zip",
                               "csv_dir_multi_zip",
                               false},
        AppendRefreshTestParam{32000000,
                               "csv",
                               "single_file.zip",
                               "csv_single_zip",
                               false},
        AppendRefreshTestParam{1, "csv", "single_file.csv", "single_csv", true},
        AppendRefreshTestParam{1,
                               "parquet",
                               "single_file.parquet",
                               "single_parquet",
                               true},
        AppendRefreshTestParam{1, "csv", "csv_dir_file", "csv_dir", true},
        AppendRefreshTestParam{1, "parquet", "parquet_dir_file", "parquet_dir", true},
        AppendRefreshTestParam{1, "csv", "dir_file.zip", "csv_dir_zip", true},
        AppendRefreshTestParam{1,
                               "csv",
                               "csv_dir_file_multi",
                               "csv_dir_file_multi",
                               true},
        AppendRefreshTestParam{1,
                               "parquet",
                               "parquet_dir_file_multi",
                               "parquet_dir_file_multi",
                               true},
        AppendRefreshTestParam{1, "csv", "dir_file_multi.zip", "csv_dir_multi_zip", true},
        AppendRefreshTestParam{1, "csv", "single_file.zip", "csv_single_zip", true}),
    PrintToStringParamName());

TEST_P(FragmentSizesAppendRefreshTest, AppendFrags) {
  auto& param = GetParam();
  int fragment_size = param.fragment_size;
  std::string filename = param.filename;

  // Create initial files and tables
  bf::remove_all(getDataFilesPath() + "append_tmp");

  recursive_copy(getDataFilesPath() + "append_before", getDataFilesPath() + "append_tmp");
  std::string file_path =
      getDataFilesPath() + "append_tmp/" + "single_file." + param.wrapper;

  std::string query = "CREATE FOREIGN TABLE " + default_name + " (i INTEGER) "s +
                      "SERVER omnisci_local_" + param.wrapper + " WITH (file_path = '" +
                      getDataFilesPath() + "append_tmp/" + filename +
                      "', fragment_size = '" + std::to_string(fragment_size) +
                      "', REFRESH_UPDATE_TYPE = 'APPEND');";
  sql(query);

  std::string select = "SELECT * FROM "s + default_name + " ORDER BY i;";
  // Read from table
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});
  bf::remove_all(getDataFilesPath() + "append_tmp");
  recursive_copy(getDataFilesPath() + "append_after", getDataFilesPath() + "append_tmp");

  if (param.recover_cache) {
    // Reset cache
    resetPersistentStorageMgr(DiskCacheLevel::fsi);
  }
  size_t mdata_count = cache_->getNumMetadataAdded();
  size_t chunk_count = cache_->getNumChunksAdded();
  // Refresh command
  sql("REFRESH FOREIGN TABLES " + default_name + ";");

  size_t original_chunks = std::ceil(double(2) / fragment_size);
  size_t final_chunks = std::ceil(double(5) / fragment_size);
  // All new chunks + last original chunk will be updated
  size_t update_count = final_chunks - original_chunks + 1;
  ASSERT_EQ(update_count, cache_->getNumMetadataAdded() - mdata_count);
  // Only last original chunk is recached
  ASSERT_EQ(1U, cache_->getNumChunksAdded() - chunk_count);

  // cache contains all original chunks
  {
    std::vector<std::vector<int>> chunk_subkeys;
    for (int i = 0; i < static_cast<int>(original_chunks); i++) {
      chunk_subkeys.push_back({1, i});
    }
    ASSERT_TRUE(does_cache_contain_chunks(&getCatalog(), default_name, chunk_subkeys));
  }
  // Check count to ensure metadata is updated
  sqlAndCompareResult("SELECT COUNT(*) FROM "s + default_name + ";", {{i(5)}});

  sqlAndCompareResult(select, {{i(1)}, {i(2)}, {i(3)}, {i(4)}, {i(5)}});
  ASSERT_EQ(update_count, cache_->getNumMetadataAdded() - mdata_count);
  // Now all new chunks are cached
  ASSERT_EQ(update_count, cache_->getNumChunksAdded() - chunk_count);
  ASSERT_EQ(param.recover_cache, isTableDatawrapperRestored(default_name));

  // cache contains all original+new chunks
  {
    std::vector<std::vector<int>> chunk_subkeys;
    for (int i = 0; i < static_cast<int>(final_chunks); i++) {
      chunk_subkeys.push_back({1, i});
    }
    ASSERT_TRUE(does_cache_contain_chunks(&getCatalog(), default_name, chunk_subkeys));
  }

  bf::remove_all(getDataFilesPath() + "append_tmp");
}

class DataWrapperAppendRefreshTest : public AppendRefreshTest {};

INSTANTIATE_TEST_SUITE_P(
    AppendParamaterizedTests,
    DataWrapperAppendRefreshTest,
    ::testing::Values(
        AppendRefreshTestParam{1, "csv", "single_file.csv", "single_csv", false},
        AppendRefreshTestParam{1,
                               "parquet",
                               "single_file.parquet",
                               "single_parquet",
                               false},
        AppendRefreshTestParam{1, "csv", "single_file.csv", "single_csv", true},
        AppendRefreshTestParam{1,
                               "parquet",
                               "single_file.parquet",
                               "single_parquet",
                               true}));

TEST_P(DataWrapperAppendRefreshTest, AppendNothing) {
  auto& param = GetParam();
  int fragment_size = 1;
  std::string filename = "single_file." + param.wrapper;

  std::string query = "CREATE FOREIGN TABLE " + default_name + " (i INTEGER) "s +
                      "SERVER omnisci_local_" + param.wrapper + " WITH (file_path = '" +
                      getDataFilesPath() + "append_before/" + filename +
                      "', fragment_size = '" + std::to_string(fragment_size) +
                      "', REFRESH_UPDATE_TYPE = 'APPEND');";
  sql(query);
  std::string select = "SELECT * FROM "s + default_name + " ORDER BY i;";
  // Read from table
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});

  if (param.recover_cache) {
    // Reset cache
    resetPersistentStorageMgr(DiskCacheLevel::fsi);
  }
  size_t mdata_count = cache_->getNumMetadataAdded();
  size_t chunk_count = cache_->getNumChunksAdded();
  // Refresh command
  sql("REFRESH FOREIGN TABLES " + default_name + ";");

  // Only last original chunk is recached
  ASSERT_EQ(1U, cache_->getNumMetadataAdded() - mdata_count);
  ASSERT_EQ(1U, cache_->getNumChunksAdded() - chunk_count);
  // Read from table
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});

  ASSERT_EQ(param.recover_cache, isTableDatawrapperRestored(default_name));

  // no updates to the cache
  ASSERT_EQ(1U, cache_->getNumMetadataAdded() - mdata_count);
  ASSERT_EQ(1U, cache_->getNumChunksAdded() - chunk_count);
}

TEST_P(DataWrapperAppendRefreshTest, MissingRows) {
  int fragment_size = 1;
  std::string wrapper = GetParam().wrapper;
  std::string filename = "single_file_delete_rows." + wrapper;
  // Create initial files and tables
  bf::remove_all(getDataFilesPath() + "append_tmp");
  recursive_copy(getDataFilesPath() + "append_before", getDataFilesPath() + "append_tmp");

  std::string query = "CREATE FOREIGN TABLE " + default_name + " (i INTEGER) "s +
                      "SERVER omnisci_local_" + wrapper + " WITH (file_path = '" +
                      getDataFilesPath() + "append_tmp/" + filename +
                      "', fragment_size = '" + std::to_string(fragment_size) +
                      "', REFRESH_UPDATE_TYPE = 'APPEND');";
  sql(query);

  std::string select = "SELECT * FROM "s + default_name + " ORDER BY i;";
  // Read from table
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});

  // Modify files
  bf::remove_all(getDataFilesPath() + "append_tmp");
  recursive_copy(getDataFilesPath() + "append_after", getDataFilesPath() + "append_tmp");

  // Refresh command
  queryAndAssertException(
      "REFRESH FOREIGN TABLES " + default_name + ";",
      "Exception: Refresh of foreign table created with \"APPEND\" update type failed as "
      "file reduced in size: " +
          getDataFilesPath() + "append_tmp/single_file_delete_rows." + wrapper);

  bf::remove_all(getDataFilesPath() + "append_tmp");
}

TEST_P(DataWrapperAppendRefreshTest, MissingFile) {
  int fragment_size = 1;
  std::string wrapper = GetParam().wrapper;
  std::string filename = wrapper + "_dir_missing_file";

  // Create initial files and tables
  bf::remove_all(getDataFilesPath() + "append_tmp");

  recursive_copy(getDataFilesPath() + "append_before", getDataFilesPath() + "append_tmp");
  std::string file_path = getDataFilesPath() + "append_tmp/" + "single_file." + wrapper;

  std::string query = "CREATE FOREIGN TABLE " + default_name + " (i INTEGER) "s +
                      "SERVER omnisci_local_" + wrapper + " WITH (file_path = '" +
                      getDataFilesPath() + "append_tmp/" + filename +
                      "', fragment_size = '" + std::to_string(fragment_size) +
                      "', REFRESH_UPDATE_TYPE = 'APPEND');";
  sql(query);

  std::string select = "SELECT * FROM "s + default_name + " ORDER BY i;";
  // Read from table
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});
  bf::remove_all(getDataFilesPath() + "append_tmp");
  recursive_copy(getDataFilesPath() + "append_after", getDataFilesPath() + "append_tmp");

  // Refresh command
  queryAndAssertException(
      "REFRESH FOREIGN TABLES " + default_name + ";",
      "Exception: Refresh of foreign table created with \"APPEND\" update type failed as "
      "file \"" +
          getDataFilesPath() + "append_tmp/" + filename + "/one_row_2." + wrapper +
          "\" was removed.");

  bf::remove_all(getDataFilesPath() + "append_tmp");
}

// This tests the use case where there are multiple files in a
// directory but an update is made to only one of the files.
TEST_P(DataWrapperAppendRefreshTest, MultifileAppendtoFile) {
  int fragment_size = 1;
  std::string wrapper = GetParam().wrapper;
  std::string filename = wrapper + "_dir_file_multi_bad_append";

  // Create initial files and tables
  bf::remove_all(getDataFilesPath() + "append_tmp");

  recursive_copy(getDataFilesPath() + "append_before", getDataFilesPath() + "append_tmp");
  std::string file_path = getDataFilesPath() + "append_tmp/" + "single_file." + wrapper;

  std::string query = "CREATE FOREIGN TABLE " + default_name + " (i INTEGER) "s +
                      "SERVER omnisci_local_" + wrapper + " WITH (file_path = '" +
                      getDataFilesPath() + "append_tmp/" + filename +
                      "', fragment_size = '" + std::to_string(fragment_size) +
                      "', REFRESH_UPDATE_TYPE = 'APPEND');";
  sql(query);

  std::string select = "SELECT * FROM "s + default_name + " ORDER BY i;";
  // Read from table
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});
  bf::remove_all(getDataFilesPath() + "append_tmp");
  recursive_copy(getDataFilesPath() + "append_after", getDataFilesPath() + "append_tmp");

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + default_name + ";");
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});

  bf::remove_all(getDataFilesPath() + "append_tmp");
}

INSTANTIATE_TEST_SUITE_P(
    DataTypeFragmentSizeAndDataWrapperParameterizedTests,
    DataTypeFragmentSizeAndDataWrapperTest,
    ::testing::Values(
        DataTypeFragmentSizeAndDataWrapperParam{1, "csv", "csv"},
        DataTypeFragmentSizeAndDataWrapperParam{1, "csv", "dir"},
        DataTypeFragmentSizeAndDataWrapperParam{1, "csv", "zip"},
        DataTypeFragmentSizeAndDataWrapperParam{1, "parquet", "parquet"},
        DataTypeFragmentSizeAndDataWrapperParam{1, "parquet", "dir"},
        DataTypeFragmentSizeAndDataWrapperParam{2, "csv", "csv"},
        DataTypeFragmentSizeAndDataWrapperParam{2, "csv", "dir"},
        DataTypeFragmentSizeAndDataWrapperParam{2, "csv", "zip"},
        DataTypeFragmentSizeAndDataWrapperParam{2, "parquet", "parquet"},
        DataTypeFragmentSizeAndDataWrapperParam{2, "parquet", "dir"},
        DataTypeFragmentSizeAndDataWrapperParam{32000000, "csv", "csv"},
        DataTypeFragmentSizeAndDataWrapperParam{32000000, "csv", "dir"},
        DataTypeFragmentSizeAndDataWrapperParam{32000000, "csv", "zip"},
        DataTypeFragmentSizeAndDataWrapperParam{32000000, "parquet", "parquet"},
        DataTypeFragmentSizeAndDataWrapperParam{32000000, "parquet", "dir"}),
    PrintToStringParamName());

TEST_P(DataTypeFragmentSizeAndDataWrapperTest, ScalarTypes) {
  auto& param = GetParam();
  int fragment_size = param.fragment_size;
  std::string data_wrapper_type = param.wrapper;
  std::string extension = param.extension;
  const auto& query = getCreateForeignTableQuery(
      "(b BOOLEAN, t TINYINT, s SMALLINT, i INTEGER, bi BIGINT, f FLOAT, "
      "dc DECIMAL(10, 5), tm TIME, tp TIMESTAMP, d DATE, txt TEXT, "
      "txt_2 TEXT ENCODING NONE)",
      {{"fragment_size", std::to_string(fragment_size)}},
      "scalar_types",
      data_wrapper_type,
      0,
      default_table_name,
      extension);
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY t;");
  // clang-format off
  assertResultSetEqual({
    {
      True, i(100), i(30000), i(2000000000), i(9000000000000000000), 10.1f, 100.1234, "00:00:10",
      "1/1/2000 00:00:59", "1/1/2000", "text_1", "quoted text"
    },
    {
      False, i(110), i(30500), i(2000500000), i(9000000050000000000), 100.12f, 2.1234, "00:10:00",
      "6/15/2020 00:59:59", "6/15/2020", "text_2", "quoted text 2"
    },
    {
      True, i(120), i(31000), i(2100000000), i(9100000000000000000), 1000.123f, 100.1, "10:00:00",
      "12/31/2500 23:59:59", "12/31/2500", "text_3", "quoted text 3"
    }},
    result);
  // clang-format on
}

TEST_F(SelectQueryTest, CsvArrayQuotedText) {
  const auto& query = getCreateForeignTableQuery(
      "(index INT, quoted_text TEXT[])", "array_quoted_text", "csv");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");
  // clang-format off
  assertResultSetEqual({
    { i(1),array({"quoted text"}) },
    { i(2),array({"quoted text 2"}) },
    { i(3),array({"quoted text 3", "quoted text 4"}) }},
    result);
  // clang-format on
}

TEST_F(SelectQueryTest, ParquetArrayDateTimeTypes) {
  const auto& query = getCreateForeignTableQuery(
      "(index INT, time_milli_array TIME[], time_micro_array TIME[],"
      " time_nano_array TIME[], timestamp_milli1_array TIMESTAMP[],"
      " timestamp_micro1_array TIMESTAMP[], timestamp_milli2_array TIMESTAMP(3)[],"
      " timestamp_micro2_array TIMESTAMP(6)[], date_array DATE[])",
      "array_datetime_types",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");

  // clang-format off
  assertResultSetEqual(
      {
          {
            i(1),array({}), array({}), array({}), array({}), array({}), array({}),
            array({}), array({})
          },
          {
            i(2),array({"23:59:59", "00:59:59", "12:00:00"}),
            array({"23:59:59", "00:59:59", "12:00:00"}),
            array({"23:59:59", "00:59:59", "12:00:00"}),
            array({"1871-07-06 23:59:59", "1931-03-01 00:59:59", "1900-12-29 12:00:00"}),
            array({"1871-07-06 23:59:59", "1931-03-01 00:59:59", "1900-12-29 12:00:00"}),
            array({"1871-07-06 23:59:59.123", "1931-03-01 00:59:59.123",
                   "1900-12-29 12:00:00.123"}), array({"1871-07-06 23:59:59.123456",
                   "1931-03-01 00:59:59.123456", "1900-12-29 12:00:00.123456"}),
            array({"1871-07-06", "1931-03-01", "1900-12-29"})
          },
          {
            i(3),array({"10:10:10", i(NULL_BIGINT)}), array({"10:10:10", i(NULL_BIGINT)}),
            array({"10:10:10", i(NULL_BIGINT)}),
            array({"2020-11-10 10:10:10", i(NULL_BIGINT)}),
            array({"2020-11-10 10:10:10", i(NULL_BIGINT)}),
            array({"2020-11-10 10:10:10.123", i(NULL_BIGINT)}),
            array({"2020-11-10 10:10:10.123456", i(NULL_BIGINT)}),
            array({"2020-11-10", i(NULL_BIGINT)})
          },
          {
            i(4),array({}), array({}), array({}),
            array({}), array({}), array({}), array({}), array({})
          },
          {
            i(5),array({"00:00:01"}),
            array({"00:00:01"}), array({"00:00:01"}), array({"2200-01-01 00:00:01"}),
            array({"2200-01-01 00:00:01"}), array({"2200-01-01 00:00:01.123"}),
            array({"2200-01-01 00:00:01.123456"}), array({"2200-01-01"})
          },
      }, result);
  // clang-format on
}

TEST_F(SelectQueryTest, DISABLED_ParquetFixedLengthArrayMalformed) {
  // TODO: to fix this test, remove the catch clause in BufferMgr::getBuffer
  // that aborts and does not thrown an exception. Alternatively, write the
  // test to expect an abort.
  const auto& query = getCreateForeignTableQuery(
      "(bigint_array BIGINT[3])", "array_fixed_len_malformed", "parquet");
  sql(query);
  TQueryResult result;
  queryAndAssertException(
      "SELECT * FROM test_foreign_table;",
      "Detected a row with 2 elements being loaded into  OmniSci column 'bigint_array' "
      "which has type fixed length array, expecting 3 elements");
}

TEST_F(SelectQueryTest, ParquetFixedLengthArrayDateTimeTypes) {
  const auto& query = getCreateForeignTableQuery(
      "(index INT, time_milli_array TIME[2], time_micro_array TIME[2],"
      " time_nano_array TIME[2], timestamp_milli1_array TIMESTAMP[2],"
      " timestamp_micro1_array TIMESTAMP[2], timestamp_milli2_array TIMESTAMP(3)[2],"
      " timestamp_micro2_array TIMESTAMP(6)[2], date_array DATE[2])",
      "array_fixed_len_datetime_types",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");

  // clang-format off
  assertResultSetEqual(
      {
          {
            i(1),array({}), array({}), array({}), array({}), array({}), array({}),
            array({}), array({})
          },
          {
            i(2),array({"23:59:59", "00:59:59"}),
            array({"23:59:59", "00:59:59"}),
            array({"23:59:59", "00:59:59"}),
            array({"1871-07-06 23:59:59", "1931-03-01 00:59:59"}),
            array({"1871-07-06 23:59:59", "1931-03-01 00:59:59"}),
            array({"1871-07-06 23:59:59.123", "1931-03-01 00:59:59.123"}),
            array({"1871-07-06 23:59:59.123456", "1931-03-01 00:59:59.123456"}),
            array({"1871-07-06", "1931-03-01"})
          },
          {
            i(3),array({"10:10:10", i(NULL_BIGINT)}), array({"10:10:10", i(NULL_BIGINT)}),
            array({"10:10:10", i(NULL_BIGINT)}),
            array({"2020-11-10 10:10:10", i(NULL_BIGINT)}),
            array({"2020-11-10 10:10:10", i(NULL_BIGINT)}),
            array({"2020-11-10 10:10:10.123", i(NULL_BIGINT)}),
            array({"2020-11-10 10:10:10.123456", i(NULL_BIGINT)}),
            array({"2020-11-10", i(NULL_BIGINT)})
          },
          {
            i(4),array({}), array({}), array({}),
            array({}), array({}), array({}), array({}), array({})
          },
          {
            i(5),array({"00:00:01", "12:00:00"}),
            array({"00:00:01", "12:00:00"}), array({"00:00:01", "12:00:00"}),
            array({"2200-01-01 00:00:01", "1900-12-29 12:00:00"}),
            array({"2200-01-01 00:00:01", "1900-12-29 12:00:00"}),
            array({"2200-01-01 00:00:01.123", "1900-12-29 12:00:00.123"}),
            array({"2200-01-01 00:00:01.123456", "1900-12-29 12:00:00.123456"}),
            array({"2200-01-01", "1900-12-29"})
          },
      }, result);
  // clang-format on
}

TEST_F(SelectQueryTest, ParquetNullCompressedGeoTypes) {
  const auto& query = getCreateForeignTableQuery(
      "( index INT, p GEOMETRY(POINT,4326) ENCODING COMPRESSED(32), l "
      "GEOMETRY(LINESTRING,4326) ENCODING COMPRESSED(32), poly GEOMETRY(POLYGON,4326) "
      "ENCODING COMPRESSED(32), mpoly GEOMETRY(MULTIPOLYGON,4326) ENCODING "
      "COMPRESSED(32) )",
      "geo_types_with_nulls",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");

  // clang-format off
  assertResultSetEqual(
  {
      {
        i(1),
        "POINT (0 0)",
        "LINESTRING (0 0,0 0)",
        "POLYGON ((0 0,0.999999940861017 0.0,0.0 0.999999982770532,0.999999940861017 "
        "0.999999982770532,0 0))",
        "MULTIPOLYGON (((0 0,0.999999940861017 0.0,0.0 0.999999982770532,0 0)))"},
      {
        i(2), "NULL", "NULL", "NULL", "NULL"
      },
      {
        i(3),
        "POINT (0.999999940861017 0.999999982770532)",
        "LINESTRING (0.999999940861017 0.999999982770532,1.99999996554106 "
        "1.99999996554106,2.99999999022111 2.99999999022111)",
        "POLYGON ((4.99999995576218 3.99999997299165,6.99999992130324 "
        "3.99999997299165,5.99999998044223 4.99999999767169,4.99999995576218 "
        "3.99999997299165))",
        "MULTIPOLYGON (((0 0,0.999999940861017 0.0,0.0 0.999999982770532,0 0)),((0 "
        "0,1.99999996554106 0.0,0.0 1.99999996554106,0 0)))",
      },
      {
        i(4),
        "POINT (1.99999996554106 1.99999996554106)",
        "LINESTRING (1.99999996554106 1.99999996554106,2.99999999022111 "
        "2.99999999022111)",
        "POLYGON ((0.999999940861017 0.999999982770532,2.99999999022111 "
        "0.999999982770532,1.99999996554106 2.99999999022111,0.999999940861017 "
        "0.999999982770532))",
        "MULTIPOLYGON (((0 0,2.99999999022111 0.0,0.0 2.99999999022111,0 0)),((0 "
        "0,0.999999940861017 0.0,0.0 0.999999982770532,0 0)),((0 0,1.99999996554106 "
        "0.0,0.0 1.99999996554106,0 0)))",
      },
      {
        i(5), "NULL", "NULL", "NULL", "NULL",
      },
  },
  result);
  // clang-format on
}

TEST_F(SelectQueryTest, ParquetNullGeoTypes) {
  const auto& query = getCreateForeignTableQuery(
      "( index INT, p POINT, l LINESTRING, poly POLYGON, mpoly MULTIPOLYGON )",
      "geo_types_with_nulls",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");

  // clang-format off
  assertResultSetEqual({
    {
      i(1), "POINT (0 0)", "LINESTRING (0 0,0 0)", "POLYGON ((0 0,1 0,0 1,1 1,0 0))",
      "MULTIPOLYGON (((0 0,1 0,0 1,0 0)))"
    },
    {
      i(2), "NULL", "NULL", "NULL", "NULL"
    },
    {
      i(3), "POINT (1 1)", "LINESTRING (1 1,2 2,3 3)", "POLYGON ((5 4,7 4,6 5,5 4))",
      "MULTIPOLYGON (((0 0,1 0,0 1,0 0)),((0 0,2 0,0 2,0 0)))"
    },
    {
      i(4), "POINT (2 2)", "LINESTRING (2 2,3 3)", "POLYGON ((1 1,3 1,2 3,1 1))",
      "MULTIPOLYGON (((0 0,3 0,0 3,0 0)),((0 0,1 0,0 1,0 0)),((0 0,2 0,0 2,0 0)))"
    },
    {
      i(5), "NULL", "NULL", "NULL", "NULL"
    }},
    result);
  // clang-format on
}

TEST_F(SelectQueryTest, ParquetGeoTypesMetadata) {
  const auto& query = getCreateForeignTableQuery(
      "( index INT, p POINT, l LINESTRING, poly POLYGON, mpoly MULTIPOLYGON )",
      "geo_types",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");

  std::map<std::pair<int, int>, std::unique_ptr<ChunkMetadata>> test_chunk_metadata_map;
  test_chunk_metadata_map[{0, 1}] = createChunkMetadata<int32_t>(1, 12, 3, 1, 3, false);
  test_chunk_metadata_map[{0, 2}] = createChunkMetadata(2, 0, 3, false);
  test_chunk_metadata_map[{0, 3}] = createChunkMetadata<int8_t>(3, 48, 3, -16, 64, false);
  test_chunk_metadata_map[{0, 4}] = createChunkMetadata(4, 0, 3, false);
  test_chunk_metadata_map[{0, 5}] =
      createChunkMetadata<int8_t>(5, 112, 3, -16, 64, false);
  test_chunk_metadata_map[{0, 6}] =
      createChunkMetadata<double>(6, 96, 3, 0.000000, 3.000000, false);
  test_chunk_metadata_map[{0, 7}] = createChunkMetadata(7, 0, 3, false);
  test_chunk_metadata_map[{0, 8}] =
      createChunkMetadata<int8_t>(8, 160, 3, -16, 64, false);
  test_chunk_metadata_map[{0, 9}] = createChunkMetadata<int32_t>(9, 12, 3, 3, 4, false);
  test_chunk_metadata_map[{0, 10}] =
      createChunkMetadata<double>(10, 96, 3, 0.000000, 7.000000, false);
  test_chunk_metadata_map[{0, 11}] = createChunkMetadata<int32_t>(11, 12, 3, 0, 0, false);
  test_chunk_metadata_map[{0, 12}] = createChunkMetadata(12, 0, 3, false);
  test_chunk_metadata_map[{0, 13}] =
      createChunkMetadata<int8_t>(13, 288, 3, -16, 64, false);
  test_chunk_metadata_map[{0, 14}] = createChunkMetadata<int32_t>(14, 24, 3, 3, 3, false);
  test_chunk_metadata_map[{0, 15}] = createChunkMetadata<int32_t>(15, 24, 3, 1, 1, false);
  test_chunk_metadata_map[{0, 16}] =
      createChunkMetadata<double>(16, 96, 3, 0.000000, 3.000000, false);
  test_chunk_metadata_map[{0, 17}] = createChunkMetadata<int32_t>(17, 12, 3, 0, 0, false);
  assertExpectedChunkMetadata(test_chunk_metadata_map);
}

TEST_F(SelectQueryTest, DISABLED_ParquetMalformedGeoPoint) {
  // TODO: to fix this test, remove the catch clause in BufferMgr::getBuffer
  // that aborts and does not thrown an exception. Alternatively, write the
  // test to expect an abort.
  const auto& query =
      getCreateForeignTableQuery("( p POINT )", "geo_point_malformed", "parquet");
  sql(query);

  TQueryResult result;
  queryAndAssertException("SELECT * FROM test_foreign_table;",
                          "Failed to extract valid geometry from row 0 for OmniSci "
                          "column p importing from parquet column point");
}

TEST_F(SelectQueryTest, DISABLED_ParquetWrongGeoType) {
  // TODO: to fix this test, remove the catch clause in BufferMgr::getBuffer
  // that aborts and does not thrown an exception. Alternatively, write the
  // test to expect an abort.
  const auto& query =
      getCreateForeignTableQuery("( p LINESTRING )", "geo_point", "parquet");
  sql(query);

  TQueryResult result;
  queryAndAssertException("SELECT * FROM test_foreign_table;",
                          "Imported geometry from parquet column point doesn't match the "
                          "geospatial type of OmniSci column p");
}

TEST_F(SelectQueryTest, ParquetArrayUnsignedIntegerTypes) {
  const auto& query = getCreateForeignTableQuery(
      "( index INT, utinyint_array SMALLINT[], usmallint_array INT[],"
      " uint_array BIGINT[] )",
      "array_unsigned_types",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");

  // clang-format off
  assertResultSetEqual( {
        {
          i(1),array({i(1),i(2)}),array({i(1),i(2)}),array({i(1),i(2)})
        },
        {
          i(2),array({i(3),i(4),i(5)}),array({i(3),i(4),i(5)}),array({i(3),i(4),i(5)})
        },
        {
          i(3),array({i(6),i(NULL_SMALLINT)}),
          array({i(6),i(NULL_INT)}),array({i(6),i(NULL_BIGINT)})
        },
        {
          i(4),array({}),array({}),array({}),array({})
        },
        {
          i(5),array({i(7)}),array({i(7)}),array({i(7)})
        }
      }, result);
  // clang-format on
}

TEST_F(SelectQueryTest, ParquetFixedLengthArrayUnsignedIntegerTypes) {
  const auto& query = getCreateForeignTableQuery(
      "( index INT, utinyint_array SMALLINT[2], usmallint_array INT[2],"
      " uint_array BIGINT[2] )",
      "array_fixed_len_unsigned_types",
      "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");

  // clang-format off
  assertResultSetEqual( {
        {
          i(1),array({i(1),i(2)}),array({i(1),i(2)}),array({i(1),i(2)})
        },
        {
          i(2),array({i(3),i(4)}),array({i(3),i(4)}),array({i(3),i(4)})
        },
        {
          i(3),array({i(6),i(NULL_SMALLINT)}),
          array({i(6),i(NULL_INT)}),array({i(6),i(NULL_BIGINT)})
        },
        {
          i(4),array({}),array({}),array({}),array({})
        },
        {
          i(5),array({i(7),i(8)}),array({i(7),i(8)}),array({i(7),i(8)})
        }
      }, result);
  // clang-format on
}

TEST_P(DataTypeFragmentSizeAndDataWrapperTest, ArrayTypes) {
  auto& param = GetParam();
  int fragment_size = param.fragment_size;
  std::string data_wrapper_type = param.wrapper;
  std::string extension = param.extension;
  const auto& query = getCreateForeignTableQuery(
      "(index INT, b BOOLEAN[], t TINYINT[], s SMALLINT[], i INTEGER[], bi BIGINT[],"
      " f FLOAT[], tm TIME[], tp TIMESTAMP[], d DATE[], txt TEXT[],"
      " fixedpoint DECIMAL(10,5)[])",
      {{"fragment_size", std::to_string(fragment_size)}},
      "array_types",
      data_wrapper_type,
      0,
      default_table_name,
      extension);
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");
  // clang-format off
  assertResultSetEqual({
    {
      i(1), array({True}), array({i(50), i(100)}), array({i(30000), i(20000)}), array({i(2000000000)}),
      array({i(9000000000000000000)}), array({10.1f, 11.1f}), array({"00:00:10"}),
      array({"1/1/2000 00:00:59", "1/1/2010 00:00:59"}), array({"1/1/2000", "2/2/2000"}),
      array({"text_1"}),array({1.23,2.34})
    },
    {
      i(2), array({False, True}), array({i(110)}), array({i(30500)}), array({i(2000500000)}),
      array({i(9000000050000000000)}), array({100.12f}), array({"00:10:00", "00:20:00"}),
      array({"6/15/2020 00:59:59"}), array({"6/15/2020"}),
      array({"text_2", "text_3"}),array({3.456,4.5,5.6})
    },
    {
      i(3), array({True}), array({i(120)}), array({i(31000)}), array({i(2100000000), i(200000000)}),
      array({i(9100000000000000000), i(9200000000000000000)}), array({1000.123f}), array({"10:00:00"}),
      array({"12/31/2500 23:59:59"}), array({"12/31/2500"}),
      array({"text_4"}),array({6.78})
    }},
    result);
  // clang-format on
}

TEST_P(DataTypeFragmentSizeAndDataWrapperTest, FixedLengthArrayTypes) {
  auto& param = GetParam();
  int fragment_size = param.fragment_size;
  std::string data_wrapper_type = param.wrapper;
  std::string extension = param.extension;
  const auto& query = getCreateForeignTableQuery(
      "(index INT, b BOOLEAN[2], t TINYINT[2], s SMALLINT[2], i INTEGER[2], bi BIGINT[2],"
      " f FLOAT[2], tm TIME[2], tp TIMESTAMP[2], d DATE[2], txt TEXT[2],"
      " fixedpoint DECIMAL(10,5)[2])",
      {{"fragment_size", std::to_string(fragment_size)}},
      "array_fixed_len_types",
      data_wrapper_type,
      0,
      default_table_name,
      extension);
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");

  // clang-format off
  assertResultSetEqual({
    {
      i(1), array({True,False}), array({i(50), i(100)}), array({i(30000), i(20000)}), array({i(2000000000),i(-100000)}),
      array({i(9000000000000000000),i(-9000000000000000000)}), array({10.1f, 11.1f}), array({"00:00:10","01:00:10"}),
      array({"1/1/2000 00:00:59", "1/1/2010 00:00:59"}), array({"1/1/2000", "2/2/2000"}),
      array({"text_1","text_2"}),array({1.23,2.34})
    },
    {
      i(2), array({False, True}), array({i(110),i(101)}), array({i(30500),i(10001)}), array({i(2000500000),i(-23233)}),
      array({i(9000000050000000000),i(-9200000000000000000)}), array({100.12f,2.22f}), array({"00:10:00", "00:20:00"}),
      array({"6/15/2020 00:59:59","8/22/2020 00:00:59"}), array({"6/15/2020","8/22/2020"}),
      array({"text_3", "text_4"}),array({3.456,4.5})
    },
    {
      i(3), array({True,True}), array({i(120),i(44)}), array({i(31000),i(8123)}), array({i(2100000000), i(200000000)}),
      array({i(9100000000000000000), i(9200000000000000000)}), array({1000.123f,1392.22f}), array({"10:00:00","20:00:00"}),
      array({"12/31/2500 23:59:59","1/1/2500 23:59:59"}), array({"12/31/2500","1/1/2500"}),
      array({"text_5","text_6"}),array({6.78,5.6})
    }},
    result);
}


TEST_P(DataTypeFragmentSizeAndDataWrapperTest, GeoTypes) {
  auto& param = GetParam();
  int fragment_size = param.fragment_size;
  std::string data_wrapper_type = param.wrapper;
  std::string extension = param.extension;

  // index column added for sorting, since order of files in a directory may vary
  const auto& query = getCreateForeignTableQuery(
      "(index int, p POINT, l LINESTRING, poly POLYGON, multipoly MULTIPOLYGON)",
      {{"fragment_size", std::to_string(fragment_size)}},
      "geo_types",
      data_wrapper_type,
      0,
      default_table_name,
      extension);
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table ORDER BY index;");
  // clang-format off
  assertResultSetEqual({
    {
      i(1), "POINT (0 0)", "LINESTRING (0 0,0 0)", "POLYGON ((0 0,1 0,0 1,1 1,0 0))",
      "MULTIPOLYGON (((0 0,1 0,0 1,0 0)))"
    },
    {
      i(2), "POINT (1 1)", "LINESTRING (1 1,2 2,3 3)", "POLYGON ((5 4,7 4,6 5,5 4))",
      "MULTIPOLYGON (((0 0,1 0,0 1,0 0)),((0 0,2 0,0 2,0 0)))"
    },
    {
      i(3), "POINT (2 2)", "LINESTRING (2 2,3 3)", "POLYGON ((1 1,3 1,2 3,1 1))",
      "MULTIPOLYGON (((0 0,3 0,0 3,0 0)),((0 0,1 0,0 1,0 0)),((0 0,2 0,0 2,0 0)))"
    }},
    result);
  // clang-format on
}

INSTANTIATE_TEST_SUITE_P(RowGroupAndFragmentSizeParameterizedTests,
                         RowGroupAndFragmentSizeSelectQueryTest,
                         ::testing::Values(std::make_pair(1, 1),
                                           std::make_pair(1, 2),
                                           std::make_pair(2, 2)),
                         PrintToStringParamName());

TEST_P(RowGroupAndFragmentSizeSelectQueryTest, MetadataOnlyCount) {
  auto param = GetParam();
  int64_t row_group_size = param.first;
  int64_t fragment_size = param.second;
  std::stringstream filename_stream;
  filename_stream << "example_row_group_size." << row_group_size;
  const auto& query =
      getCreateForeignTableQuery("(a INTEGER, b INTEGER, c INTEGER, d DOUBLE)",
                                 {{"fragment_size", std::to_string(fragment_size)}},
                                 filename_stream.str(),
                                 "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT COUNT(*) FROM test_foreign_table;");
  assertResultSetEqual({{i(6)}}, result);
}

TEST_P(RowGroupAndFragmentSizeSelectQueryTest, MetadataOnlyFilter) {
  auto param = GetParam();
  int64_t row_group_size = param.first;
  int64_t fragment_size = param.second;
  std::stringstream filename_stream;
  filename_stream << "example_row_group_size." << row_group_size;
  const auto& query =
      getCreateForeignTableQuery("(a INTEGER, b INTEGER, c INTEGER, d DOUBLE)",
                                 {{"fragment_size", std::to_string(fragment_size)}},
                                 filename_stream.str(),
                                 "parquet");
  sql(query);

  {
    TQueryResult result;
    sql(result, "SELECT COUNT(*) FROM test_foreign_table WHERE a > 2;");
    assertResultSetEqual({{i(4)}}, result);
  }

  {
    TQueryResult result;
    sql(result, "SELECT COUNT(*) FROM test_foreign_table WHERE d < 0;");
    assertResultSetEqual({{i(2)}}, result);
  }
}

TEST_P(RowGroupAndFragmentSizeSelectQueryTest, Join) {
  auto param = GetParam();
  int64_t row_group_size = param.first;
  int64_t fragment_size = param.second;
  std::stringstream filename_stream;
  filename_stream << "example_row_group_size." << row_group_size;
  auto query =
      getCreateForeignTableQuery("(a INTEGER, b INTEGER, c INTEGER, d DOUBLE)",
                                 {{"fragment_size", std::to_string(fragment_size)}},
                                 filename_stream.str(),
                                 "parquet");
  sql(query);
  query = getCreateForeignTableQuery(
      "(t TEXT, i INTEGER, d DOUBLE)", "example_2", "parquet", 2);
  sql(query);

  TQueryResult result;
  sql(result,
      "SELECT t1.a, t1.b, t1.c, t1.d, t2.i, t2.d FROM "
      "test_foreign_table AS t1 JOIN "
      "test_foreign_table_2 AS t2 ON t1.a = t2.i ORDER BY t1.a;");

  assertResultSetEqual({{i(1), i(3), i(6), 7.1, i(1), 1.1},
                        {i(1), i(3), i(6), 7.1, i(1), 1.1},
                        {i(1), i(3), i(6), 7.1, i(1), 1.1},
                        {i(2), i(4), i(7), 0.000591, i(2), 2.2},
                        {i(2), i(4), i(7), 0.000591, i(2), 2.2},
                        {i(3), i(5), i(8), 1.1, i(3), 3.3}},
                       result);
}

TEST_P(RowGroupAndFragmentSizeSelectQueryTest, Select) {
  auto param = GetParam();
  int64_t row_group_size = param.first;
  int64_t fragment_size = param.second;
  std::stringstream filename_stream;
  filename_stream << "example_row_group_size." << row_group_size;
  const auto& query =
      getCreateForeignTableQuery("(a INTEGER, b INTEGER, c INTEGER, d DOUBLE)",
                                 {{"fragment_size", std::to_string(fragment_size)}},
                                 filename_stream.str(),
                                 "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table;");
  assertResultSetEqual({{i(1), i(3), i(6), 7.1},
                        {i(2), i(4), i(7), 0.000591},
                        {i(3), i(5), i(8), 1.1},
                        {i(4), i(6), i(9), 0.022123},
                        {i(5), i(7), i(10), -1.},
                        {i(6), i(8), i(1), -100.}},
                       result);
}

TEST_P(RowGroupAndFragmentSizeSelectQueryTest, Filter) {
  auto param = GetParam();
  int64_t row_group_size = param.first;
  int64_t fragment_size = param.second;
  std::stringstream filename_stream;
  filename_stream << "example_row_group_size." << row_group_size;
  const auto& query =
      getCreateForeignTableQuery("(a INTEGER, b INTEGER, c INTEGER, d DOUBLE)",
                                 {{"fragment_size", std::to_string(fragment_size)}},
                                 filename_stream.str(),
                                 "parquet");
  sql(query);

  TQueryResult result;
  sql(result, "SELECT * FROM test_foreign_table WHERE d < 0 ;");
  assertResultSetEqual({{i(5), i(7), i(10), -1.}, {i(6), i(8), i(1), -100.}}, result);
}

using namespace foreign_storage;
class ForeignStorageCacheQueryTest : public ForeignTableTest {
 protected:
  inline static const std::string table_2_filename = "example_2";
  inline static const std::string col_name1 = "col1";
  inline static const std::string col_name2 = "col2";
  inline static const std::string col_name3 = "col3";
  inline static Catalog_Namespace::Catalog* cat;
  inline static ForeignStorageCache* cache;
  inline static File_Namespace::GlobalFileMgr* gfm;
  inline static const TableDescriptor* td;
  inline static const ColumnDescriptor *cd1, *cd2, *cd3;
  inline static ChunkKey query_chunk_key1, query_chunk_key2, query_chunk_key3,
      query_table_prefix;

  static void SetUpTestSuite() {
    DBHandlerTestFixture::SetUpTestSuite();
    cat = &getCatalog();
    cache = cat->getDataMgr().getPersistentStorageMgr()->getDiskCache();
    gfm = cache->getGlobalFileMgr();
    sqlDropForeignTable();
  }

  static void TearDownTestSuite() { DBHandlerTestFixture::TearDownTestSuite(); }

  static void createTestTable() {
    sqlCreateForeignTable(
        "(" + col_name1 + " TEXT, " + col_name2 + " INTEGER, " + col_name3 + " DOUBLE)",
        table_2_filename,
        "csv");
    td = cat->getMetadataForTable(default_table_name);
    cd1 = cat->getMetadataForColumn(td->tableId, col_name1);
    cd2 = cat->getMetadataForColumn(td->tableId, col_name2);
    cd3 = cat->getMetadataForColumn(td->tableId, col_name3);
    query_chunk_key1 = {cat->getCurrentDB().dbId, td->tableId, cd1->columnId, 0};
    query_chunk_key2 = {cat->getCurrentDB().dbId, td->tableId, cd2->columnId, 0};
    query_chunk_key3 = {cat->getCurrentDB().dbId, td->tableId, cd3->columnId, 0};
    query_table_prefix = {cat->getCurrentDB().dbId, td->tableId};
  }

  static void sqlSelect(const std::string& columns = "*",
                        const std::string& table_name = "test_foreign_table") {
    sql("SELECT " + columns + " FROM " + table_name + ";");
  }

  void SetUp() override {
    ForeignTableTest::SetUp();
    cache->clear();
    createTestTable();
  }

  void TearDown() override {
    sqlDropForeignTable();
    ForeignTableTest::TearDown();
  }
};

TEST_F(ForeignStorageCacheQueryTest, CreatePopulateMetadata) {
  sqlDropForeignTable();
  ASSERT_FALSE(cache->isMetadataCached(query_chunk_key1));
  ASSERT_FALSE(cache->isMetadataCached(query_chunk_key2));
  ASSERT_FALSE(cache->isMetadataCached(query_chunk_key3));
  ASSERT_FALSE(cache->hasCachedMetadataForKeyPrefix(query_chunk_key1));
  ASSERT_FALSE(cache->hasCachedMetadataForKeyPrefix(query_table_prefix));
  createTestTable();
  ASSERT_TRUE(cache->isMetadataCached(query_chunk_key1));
  ASSERT_TRUE(cache->isMetadataCached(query_chunk_key2));
  ASSERT_TRUE(cache->isMetadataCached(query_chunk_key3));
  ASSERT_TRUE(cache->hasCachedMetadataForKeyPrefix(query_chunk_key1));
  ASSERT_TRUE(cache->hasCachedMetadataForKeyPrefix(query_table_prefix));
}

TEST_F(ForeignStorageCacheQueryTest, CacheEvictAfterDrop) {
  sqlSelect();
  ASSERT_EQ(cache->getNumCachedChunks(), 3U);
  ASSERT_EQ(cache->getNumCachedMetadata(), 3U);
  sqlDropForeignTable();
  ASSERT_EQ(cache->getNumCachedChunks(), 0U);
  ASSERT_EQ(cache->getNumCachedMetadata(), 0U);
}

TEST_F(ForeignStorageCacheQueryTest, WideLogicalColumns) {
  cache->clear();
  ASSERT_EQ(cache->getNumCachedChunks(), 0U);
  ASSERT_EQ(cache->getNumCachedMetadata(), 0U);
  sqlDropForeignTable();
  sqlCreateForeignTable("(t TEXT, i INTEGER[])", "example_1", "csv");
  sqlSelect();
  // Metadata and chunk size differ because the INTEGER[] logical col expands into two
  // physical columns.
  ASSERT_EQ(cache->getNumCachedChunks(), 3U);
  ASSERT_EQ(cache->getNumCachedMetadata(), 2U);
  sqlDropForeignTable();
}

TEST_F(ForeignStorageCacheQueryTest, CacheWithLimitCachesWholeChunks) {
  sqlDropForeignTable();
  sqlCreateForeignTable(
      "(i INTEGER,  txt TEXT, txt_2 TEXT ENCODING NONE)", "0_255", "csv");
  sqlAndCompareResult("SELECT SUM(i) FROM (SELECT i FROM " + default_table_name +
                          " ORDER BY i LIMIT 10);",
                      {{i(45)}});
  sqlAndCompareResult("SELECT SUM(i) FROM " + default_table_name + ";", {{i(32640)}});
}

class CacheDefaultTest : public DBHandlerTestFixture {};
TEST_F(CacheDefaultTest, Path) {
  auto cat = &getCatalog();
  auto cache = cat->getDataMgr().getPersistentStorageMgr()->getDiskCache();
  ASSERT_EQ(cache->getGlobalFileMgr()->getBasePath(),
            to_string(BASE_PATH) + "/omnisci_disk_cache/");
}

TEST_F(RecoverCacheQueryTest, RecoverWithoutWrappers) {
  sqlDropForeignTable();
  sqlCreateForeignTable("(col1 INTEGER)", "1", "csv");

  auto td = cat_->getMetadataForTable(default_table_name);
  ChunkKey key{cat_->getCurrentDB().dbId, td->tableId, 1, 0};
  ChunkKey table_key{cat_->getCurrentDB().dbId, td->tableId};

  sqlAndCompareResult("SELECT * FROM " + default_table_name + ";", {{i(1)}});
  // Cache is now populated.

  // Reset cache and clear memory representations.
  resetStorageManagerAndClearTableMemory(table_key);

  // Cache should be empty until query prompts recovery from disk
  ASSERT_EQ(cache_->getNumCachedMetadata(), 0U);
  ASSERT_EQ(cache_->getNumCachedChunks(), 0U);

  // This query should hit recovered disk data and not need to create datawrappers.
  sqlAndCompareResult("SELECT * FROM " + default_table_name + ";", {{i(1)}});

  ASSERT_EQ(cache_->getNumCachedMetadata(), 1U);
  ASSERT_EQ(cache_->getNumCachedChunks(), 1U);

  // Datawrapper should not have been created.
  ASSERT_FALSE(psm_->getForeignStorageMgr()->hasDataWrapperForChunk(key));

  sqlDropForeignTable();
}

TEST_F(RecoverCacheQueryTest, RecoverThenPopulateDataWrappersOnDemandVarLen) {
  sqlDropForeignTable();
  std::string query = "CREATE FOREIGN TABLE " + default_table_name +
                      " (t TEXT, i INTEGER[]) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "/" + "example_1_dir_archives/');";
  sql(query);
  auto td = cat_->getMetadataForTable(default_table_name);
  ChunkKey key{cat_->getCurrentDB().dbId, td->tableId, 1, 0};
  ChunkKey table_key{cat_->getCurrentDB().dbId, td->tableId};

  sqlAndCompareResult("SELECT COUNT(*) FROM " + default_table_name + ";", {{i(3)}});
  ASSERT_FALSE(isTableDatawrapperRestored(default_table_name));

  // Reset cache and clear memory representations.
  resetStorageManagerAndClearTableMemory(table_key);

  // Cache should be empty until query prompts recovery from disk
  ASSERT_EQ(cache_->getNumCachedMetadata(), 0U);
  ASSERT_EQ(cache_->getNumCachedChunks(), 0U);

  ASSERT_TRUE(isTableDatawrapperDataOnDisk(default_table_name));
  ASSERT_TRUE(compareTableDatawrapperMetadataToFile(default_table_name,
                                                    getWrapperMetadataPath("example_1")));

  sqlAndCompareResult("SELECT * FROM " + default_table_name + "  ORDER BY t;",
                      {{"a", array({i(1), i(1), i(1)})},
                       {"aa", array({Null_i, i(2), i(2)})},
                       {"aaa", array({i(3), Null_i, i(3)})}});

  // 2 data + 1 index chunk
  ASSERT_EQ(cache_->getNumCachedChunks(), 3U);
  // Only 2 metadata
  ASSERT_EQ(cache_->getNumCachedMetadata(), 2U);

  ASSERT_TRUE(isTableDatawrapperRestored(default_table_name));
  sqlDropForeignTable();
}

// Check that csv datawrapper metadata is generated and restored correctly for CSV
// Archives
TEST_F(RecoverCacheQueryTest, RecoverThenPopulateDataWrappersOnDemandFromCsvArchive) {
  sqlDropForeignTable();
  std::string query = "CREATE FOREIGN TABLE " + default_table_name +
                      " (t TEXT, i INTEGER[]) "s +
                      "SERVER omnisci_local_csv WITH (file_path = '" +
                      getDataFilesPath() + "/" + "example_1_multilevel.zip');";
  sql(query);
  auto td = cat_->getMetadataForTable(default_table_name);
  ChunkKey key{cat_->getCurrentDB().dbId, td->tableId, 1, 0};
  ChunkKey table_key{cat_->getCurrentDB().dbId, td->tableId};

  sqlAndCompareResult("SELECT COUNT(*) FROM " + default_table_name + ";", {{i(3)}});

  ASSERT_FALSE(isTableDatawrapperRestored(default_table_name));
  ASSERT_TRUE(isTableDatawrapperDataOnDisk(default_table_name));
  ASSERT_TRUE(compareTableDatawrapperMetadataToFile(
      default_table_name, getWrapperMetadataPath("example_1_archive")));

  // Reset cache and clear memory representations.
  resetStorageManagerAndClearTableMemory(table_key);

  // Cache should be empty until query prompts recovery from disk
  ASSERT_EQ(cache_->getNumCachedMetadata(), 0U);
  ASSERT_EQ(cache_->getNumCachedChunks(), 0U);

  sqlAndCompareResult("SELECT * FROM " + default_table_name + "  ORDER BY t;",
                      {{"a", array({i(1), i(1), i(1)})},
                       {"aa", array({Null_i, i(2), i(2)})},
                       {"aaa", array({i(3), Null_i, i(3)})}});
  // 2 columns
  ASSERT_EQ(cache_->getNumCachedMetadata(), 2U);
  // extra chunk for varlen
  ASSERT_EQ(cache_->getNumCachedChunks(), 3U);

  ASSERT_TRUE(isTableDatawrapperRestored(default_table_name));
  sqlDropForeignTable();
}

class DataWrapperRecoverCacheQueryTest
    : public RecoverCacheQueryTest,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(DataWrapperRecoverCacheQueryTest, RecoverThenPopulateDataWrappersOnDemand) {
  auto wrapper = GetParam();

  sqlDropForeignTable();
  sqlCreateForeignTable("(col1 INTEGER)", "1", wrapper);

  auto td = cat_->getMetadataForTable(default_table_name);
  ChunkKey key{cat_->getCurrentDB().dbId, td->tableId, 1, 0};
  ChunkKey table_key{cat_->getCurrentDB().dbId, td->tableId};

  sqlAndCompareResult("SELECT COUNT(*) FROM " + default_table_name + ";", {{i(1)}});
  // Cache now has metadata only.
  ASSERT_EQ(cache_->getNumCachedMetadata(), 1U);
  ASSERT_EQ(cache_->getNumCachedChunks(), 0U);
  ASSERT_TRUE(psm_->getForeignStorageMgr()->hasDataWrapperForChunk(key));

  // Reset cache and clear memory representations.
  resetStorageManagerAndClearTableMemory(table_key);

  // Cache should be empty until query prompts recovery from disk
  ASSERT_EQ(cache_->getNumCachedMetadata(), 0U);
  ASSERT_EQ(cache_->getNumCachedChunks(), 0U);

  ASSERT_TRUE(isTableDatawrapperDataOnDisk(default_table_name));
  ASSERT_TRUE(compareTableDatawrapperMetadataToFile(
      default_table_name, getWrapperMetadataPath("1", wrapper)));

  // This query should hit recovered disk data and not need to create datawrappers.
  sqlAndCompareResult("SELECT COUNT(*) FROM " + default_table_name + ";", {{i(1)}});

  ASSERT_EQ(cache_->getNumCachedMetadata(), 1U);
  ASSERT_EQ(cache_->getNumCachedChunks(), 0U);
  ASSERT_FALSE(psm_->getForeignStorageMgr()->hasDataWrapperForChunk(key));

  sqlAndCompareResult("SELECT * FROM " + default_table_name + ";", {{i(1)}});
  ASSERT_EQ(cache_->getNumCachedChunks(), 1U);
  ASSERT_TRUE(psm_->getForeignStorageMgr()->hasDataWrapperForChunk(key));

  sqlDropForeignTable();
}

// Check that datawrapper metadata is generated and restored correctly when appending
// data
TEST_P(DataWrapperRecoverCacheQueryTest, AppendData) {
  int fragment_size = 2;
  auto wrapper = GetParam();
  std::string filename = wrapper + "_dir_file_multi";
  sqlDropForeignTable();
  // Create initial files and tables
  bf::remove_all(getDataFilesPath() + "append_tmp");

  recursive_copy(getDataFilesPath() + "append_before", getDataFilesPath() + "append_tmp");
  std::string file_path = getDataFilesPath() + "append_tmp/" + "single_file." + wrapper;

  std::string query = "CREATE FOREIGN TABLE " + default_table_name + " (i INTEGER) "s +
                      "SERVER omnisci_local_" + wrapper + " WITH (file_path = '" +
                      getDataFilesPath() + "append_tmp/" + filename +
                      "', fragment_size = '" + std::to_string(fragment_size) +
                      "', REFRESH_UPDATE_TYPE = 'APPEND');";
  sql(query);

  auto td = cat_->getMetadataForTable(default_table_name);
  ChunkKey key{cat_->getCurrentDB().dbId, td->tableId, 1, 0};
  ChunkKey table_key{cat_->getCurrentDB().dbId, td->tableId};

  std::string select = "SELECT * FROM "s + default_table_name + " ORDER BY i;";
  // Read from table
  sqlAndCompareResult(select, {{i(1)}, {i(2)}});

  ASSERT_TRUE(isTableDatawrapperDataOnDisk(default_table_name));
  ASSERT_TRUE(compareTableDatawrapperMetadataToFile(
      default_table_name, getWrapperMetadataPath("append_before", wrapper)));

  // Reset cache and clear memory representations.
  resetStorageManagerAndClearTableMemory(table_key);

  // Cache should be empty until query prompts recovery from disk
  ASSERT_EQ(cache_->getNumCachedMetadata(), 0U);
  ASSERT_EQ(cache_->getNumCachedChunks(), 0U);

  // Modify tables on disk
  bf::remove_all(getDataFilesPath() + "append_tmp");
  recursive_copy(getDataFilesPath() + "append_after", getDataFilesPath() + "append_tmp");

  // Refresh command
  sql("REFRESH FOREIGN TABLES " + default_table_name + ";");
  // Read new data
  sqlAndCompareResult(select, {{i(1)}, {i(2)}, {i(3)}, {i(4)}, {i(5)}});

  // Metadata file should be updated
  ASSERT_TRUE(isTableDatawrapperDataOnDisk(default_table_name));
  ASSERT_TRUE(compareTableDatawrapperMetadataToFile(
      default_table_name, getWrapperMetadataPath("append_after", wrapper)));

  bf::remove_all(getDataFilesPath() + "append_tmp");
  sqlDropForeignTable();
}

INSTANTIATE_TEST_SUITE_P(DataWrapperRecoverCacheQueryTest,
                         DataWrapperRecoverCacheQueryTest,
                         ::testing::Values("csv", "parquet"));

class MockDataWrapper : public foreign_storage::MockForeignDataWrapper {
 public:
  MockDataWrapper() : throw_on_metadata_scan_(false), throw_on_chunk_fetch_(false) {}

  void populateChunkMetadata(ChunkMetadataVector& chunk_metadata_vector) override {
    if (throw_on_metadata_scan_) {
      throw std::runtime_error{"populateChunkMetadata mock exception"};
    } else {
      parent_data_wrapper_->populateChunkMetadata(chunk_metadata_vector);
    }
  }

  void populateChunkBuffers(
      std::map<ChunkKey, AbstractBuffer*>& required_buffers,
      std::map<ChunkKey, AbstractBuffer*>& optional_buffers) override {
    if (throw_on_chunk_fetch_) {
      throw std::runtime_error{"populateChunkBuffers mock exception"};
    } else {
      parent_data_wrapper_->populateChunkBuffers(required_buffers, optional_buffers);
    }
  }

  void setParentWrapper(
      std::shared_ptr<ForeignDataWrapper> parent_data_wrapper) override {
    parent_data_wrapper_ = parent_data_wrapper;
  }

  void throwOnMetadataScan(bool throw_on_metadata_scan) {
    throw_on_metadata_scan_ = throw_on_metadata_scan;
  }

  void throwOnChunkFetch(bool throw_on_chunk_fetch) {
    throw_on_chunk_fetch_ = throw_on_chunk_fetch;
  }

  void serializeDataWrapperInternals(const std::string& file_path) const override{};

  void restoreDataWrapperInternals(const std::string& file_path,
                                   const ChunkMetadataVector& chunk_metadata) override{};

  bool isRestored() const override { return false; };

 private:
  std::shared_ptr<foreign_storage::ForeignDataWrapper> parent_data_wrapper_;
  std::atomic<bool> throw_on_metadata_scan_;
  std::atomic<bool> throw_on_chunk_fetch_;
};

class ScheduledRefreshTest : public RefreshTests {
 protected:
  static void SetUpTestSuite() {
    createDBHandler();
    foreign_storage::ForeignTableRefreshScheduler::setWaitDuration(1);
  }

  static void TearDownTestSuite() { stopScheduler(); }

  static void startScheduler() {
    is_program_running_ = true;
    foreign_storage::ForeignTableRefreshScheduler::start(is_program_running_);
    ASSERT_TRUE(foreign_storage::ForeignTableRefreshScheduler::isRunning());
  }

  static void stopScheduler() {
    is_program_running_ = false;
    foreign_storage::ForeignTableRefreshScheduler::stop();
    ASSERT_FALSE(foreign_storage::ForeignTableRefreshScheduler::isRunning());
  }

  void SetUp() override {
    ForeignTableTest::SetUp();
    boost::filesystem::create_directory(REFRESH_TEST_DIR);
    sql("DROP FOREIGN TABLE IF EXISTS test_foreign_table;");
    foreign_storage::ForeignTableRefreshScheduler::resetHasRefreshedTable();
    startScheduler();
  }

  void TearDown() override {
    sql("DROP FOREIGN TABLE IF EXISTS test_foreign_table;");
    boost::filesystem::remove_all(REFRESH_TEST_DIR);
    ForeignTableTest::TearDown();
  }

  void setTestFile(const std::string& file_name) {
    bf::copy_file(getDataFilesPath() + "/" + file_name,
                  REFRESH_TEST_DIR + "/test.csv",
                  bf::copy_option::overwrite_if_exists);
  }

  std::string getCreateScheduledRefreshTableQuery(
      const std::string& refresh_interval,
      const std::string& update_type = "all",
      int sec_from_now = 1,
      const std::string& timing_type = "scheduled") {
    std::time_t timestamp = getCurrentTime() + sec_from_now;
    std::tm* gmt_time = std::gmtime(&timestamp);
    constexpr int buffer_size = 256;
    char buffer[buffer_size];
    std::strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", gmt_time);
    std::string start_date_time = buffer;

    auto test_file_path = boost::filesystem::canonical(REFRESH_TEST_DIR) / "test.csv";
    std::string query =
        "CREATE FOREIGN TABLE test_foreign_table (i INTEGER) server "
        "omnisci_local_csv with (file_path = '" +
        test_file_path.string() + "', refresh_update_type = '" + update_type +
        "', refresh_timing_type = '" + timing_type + "', refresh_start_date_time = '" +
        start_date_time + "'";
    if (!refresh_interval.empty()) {
      query += ", refresh_interval = '" + refresh_interval + "'";
    }
    query += ");";
    return query;
  }

  void waitForSchedulerRefresh(bool reset_refreshed_table_flag = true) {
    if (foreign_storage::ForeignTableRefreshScheduler::isRunning()) {
      constexpr size_t max_check_count = 10;
      size_t count = 0;
      if (reset_refreshed_table_flag) {
        foreign_storage::ForeignTableRefreshScheduler::resetHasRefreshedTable();
      }
      while (!foreign_storage::ForeignTableRefreshScheduler::hasRefreshedTable() &&
             count < max_check_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        count++;
      }
      if (!foreign_storage::ForeignTableRefreshScheduler::hasRefreshedTable()) {
        throw std::runtime_error{
            "Max wait time for scheduled table refresh has been exceeded."};
      }
    }
  }

  /**
   * For some test cases, a wait is done for two refresh cycles in order to ensure
   * that a refresh is done, at least once, using new file content. For instance,
   * if a test case executes the following sequence of operations:
   * 1. Update foreign table file content
   * 2. Wait for scheduled refresh to complete
   * 3. Query foreign table and assert new content
   *
   * Step 3 may return old content if the last scheduled refresh began before
   * and ended after step 1. Running step 2 twice ensures that, in this case,
   * a second refresh that picks up new file content occurs before running the
   * query in step 3.
   */
  void waitTwoRefreshCycles() {
    waitForSchedulerRefresh();
    waitForSchedulerRefresh();
  }

  inline static const std::string REFRESH_TEST_DIR{"./fsi_scheduled_refresh_test"};
  inline static std::atomic<bool> is_program_running_;
};

TEST_F(ScheduledRefreshTest, BatchMode) {
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("1S");
  sql(query);
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(0)}});

  setTestFile("1.csv");
  waitTwoRefreshCycles();

  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(1)}});
}

TEST_F(ScheduledRefreshTest, AppendMode) {
  setTestFile("1.csv");
  auto query = getCreateScheduledRefreshTableQuery("1S", "append");
  sql(query);
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(1)}});

  setTestFile("two_row_1_2.csv");
  waitTwoRefreshCycles();

  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(1)}, {i(2)}});
}

TEST_F(ScheduledRefreshTest, OnlyStartDateTime) {
  stopScheduler();
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("", "all");
  sql(query);
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(0)}});

  setTestFile("1.csv");
  startScheduler();
  waitForSchedulerRefresh(false);
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(1)}});
}

TEST_F(ScheduledRefreshTest, StartDateTimeInThePast) {
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("1S", "all", -60);
  queryAndAssertException(
      query, "Exception: REFRESH_START_DATE_TIME cannot be a past date time.");
}

TEST_F(ScheduledRefreshTest, SecondsInterval) {
  stopScheduler();
  auto start_time = getCurrentTime();
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("10S");
  sql(query);

  startScheduler();
  waitForSchedulerRefresh(false);
  auto refresh_end_time = getCurrentTime();

  // Next refresh should be set based on interval
  auto [last_refresh_time, next_refresh_time] = getLastAndNextRefreshTimes();
  assertRefreshTimeBetween(last_refresh_time, start_time, refresh_end_time);
  constexpr int interval_duration = 10;
  assertRefreshTimeBetween(
      next_refresh_time, start_time, refresh_end_time + interval_duration);
}

TEST_F(ScheduledRefreshTest, HoursInterval) {
  stopScheduler();
  auto start_time = getCurrentTime();
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("10H");
  sql(query);

  startScheduler();
  waitForSchedulerRefresh(false);
  auto refresh_end_time = getCurrentTime();

  // Next refresh should be set based on interval
  auto [last_refresh_time, next_refresh_time] = getLastAndNextRefreshTimes();
  assertRefreshTimeBetween(last_refresh_time, start_time, refresh_end_time);
  constexpr int interval_duration = 10 * 60 * 60;
  assertRefreshTimeBetween(
      next_refresh_time, start_time, refresh_end_time + interval_duration);
}

TEST_F(ScheduledRefreshTest, DaysInterval) {
  stopScheduler();
  auto start_time = getCurrentTime();
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("10D");
  sql(query);

  startScheduler();
  waitForSchedulerRefresh(false);
  auto refresh_end_time = getCurrentTime();

  // Next refresh should be set based on interval
  auto [last_refresh_time, next_refresh_time] = getLastAndNextRefreshTimes();
  assertRefreshTimeBetween(last_refresh_time, start_time, refresh_end_time);
  constexpr int interval_duration = 10 * 60 * 60 * 24;
  assertRefreshTimeBetween(
      next_refresh_time, start_time, refresh_end_time + interval_duration);
}

TEST_F(ScheduledRefreshTest, InvalidInterval) {
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("10A");
  queryAndAssertException(
      query, "Exception: Invalid value provided for the REFRESH_INTERVAL option.");
}

TEST_F(ScheduledRefreshTest, InvalidRefreshTimingType) {
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("1S", "all", 1, "invalid");
  queryAndAssertException(query,
                          "Exception: Invalid value provided for the REFRESH_TIMING_TYPE "
                          "option. Value must be \"MANUAL\" or \"SCHEDULED\".");
}

TEST_F(ScheduledRefreshTest, MissingStartDateTime) {
  setTestFile("0.csv");
  auto test_file_path = boost::filesystem::canonical(REFRESH_TEST_DIR) / "test.csv";
  std::string query =
      "CREATE FOREIGN TABLE test_foreign_table (i INTEGER) "
      "server omnisci_local_csv with (file_path = '" +
      test_file_path.string() +
      "', "
      "refresh_timing_type = 'scheduled');";
  queryAndAssertException(query,
                          "Exception: REFRESH_START_DATE_TIME option must be provided "
                          "for scheduled refreshes.");
}

TEST_F(ScheduledRefreshTest, InvalidStartDateTime) {
  setTestFile("0.csv");
  auto test_file_path = boost::filesystem::canonical(REFRESH_TEST_DIR) / "test.csv";
  std::string query =
      "CREATE FOREIGN TABLE test_foreign_table (i INTEGER) "
      "server omnisci_local_csv with (file_path = '" +
      test_file_path.string() +
      "', "
      "refresh_timing_type = 'scheduled', refresh_start_date_time = "
      "'invalid_date_time');";
  queryAndAssertException(query,
                          "Exception: Invalid DATE/TIMESTAMP string (invalid_date_time)");
}

TEST_F(ScheduledRefreshTest, SchedulerStop) {
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("1S");
  sql(query);
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(0)}});

  stopScheduler();
  setTestFile("1.csv");
  waitForSchedulerRefresh();
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(0)}});

  startScheduler();
  setTestFile("1.csv");
  waitForSchedulerRefresh();
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(1)}});
}

// TODO: Investigate why this test case fails intermittently on
// MacOS builds and re-enable after
TEST_F(ScheduledRefreshTest, DISABLED_PreEvictionError) {
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("1S");
  sql(query);

  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(0)}});

  auto& catalog = getCatalog();
  auto foreign_storage_mgr =
      catalog.getDataMgr().getPersistentStorageMgr()->getForeignStorageMgr();
  auto table = catalog.getMetadataForTable("test_foreign_table", false);

  auto mock_data_wrapper = std::make_shared<MockDataWrapper>();
  mock_data_wrapper->throwOnMetadataScan(true);
  foreign_storage_mgr->setDataWrapper({catalog.getCurrentDB().dbId, table->tableId},
                                      mock_data_wrapper);
  setTestFile("1.csv");
  waitTwoRefreshCycles();

  // Assert that stale cached data is still used
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(0)}});
}

// This currently results in an assertion failure because the cache
// file buffer encoder is deleted when the exception occurs and
// subsequent cache method calls attempt to access the encoder.
// TODO: Look into individual cache buffer encoder recovery
// or an alternate solution that does not rely on buffer encoder
// resets.
TEST_F(ScheduledRefreshTest, DISABLED_PostEvictionError) {
  setTestFile("0.csv");
  auto query = getCreateScheduledRefreshTableQuery("1S");
  sql(query);
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(0)}});

  auto& catalog = getCatalog();
  auto foreign_storage_mgr =
      catalog.getDataMgr().getPersistentStorageMgr()->getForeignStorageMgr();
  auto table = catalog.getMetadataForTable("test_foreign_table", false);

  auto mock_data_wrapper = std::make_shared<MockDataWrapper>();
  mock_data_wrapper->throwOnChunkFetch(true);
  foreign_storage_mgr->setDataWrapper({catalog.getCurrentDB().dbId, table->tableId},
                                      mock_data_wrapper);
  setTestFile("1.csv");
  waitTwoRefreshCycles();
  mock_data_wrapper->throwOnChunkFetch(false);

  // Assert that new data is fetched
  sqlAndCompareResult("SELECT * FROM test_foreign_table;", {{i(1)}});
}

int main(int argc, char** argv) {
  g_enable_fsi = true;
  TestHelpers::init_logger_stderr_only(argc, argv);
  testing::InitGoogleTest(&argc, argv);

  // get dirname of test binary
  test_binary_file_path = bf::canonical(argv[0]).parent_path().string();

  int err{0};
  try {
    err = RUN_ALL_TESTS();
  } catch (const std::exception& e) {
    LOG(ERROR) << e.what();
  }

  g_enable_fsi = false;
  return err;
}
