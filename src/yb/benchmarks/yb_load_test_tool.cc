// Copyright (c) YugaByte, Inc.

#include <glog/logging.h>

#include <boost/bind.hpp>
#include <boost/thread/mutex.hpp>
#include <queue>
#include <set>
#include <atomic>

#include "yb/benchmarks/tpch/line_item_tsv_importer.h"
#include "yb/benchmarks/tpch/rpc_line_item_dao.h"
#include "yb/common/common.pb.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/master.h"
#include "yb/master/master.pb.h"
#include "yb/util/atomic.h"
#include "yb/util/env.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/stopwatch.h"
#include "yb/util/subprocess.h"
#include "yb/util/threadpool.h"

#include "yb/integration-tests/load_generator.h"

DEFINE_int32(rpc_timeout_sec, 30, "Timeout for RPC calls, in seconds");

DEFINE_int32(num_iter, 1, "Run the entire test this number of times");

DEFINE_string(load_test_master_addresses,
              "localhost",
              "Addresses of masters for the cluster to operate on");

DEFINE_string(table_name, "yb_load_test", "Table name to use for YugaByte load testing");

DEFINE_int64(num_rows, 50000, "Number of rows to insert");

DEFINE_int32(num_writer_threads, 4, "Number of writer threads");

DEFINE_int32(num_reader_threads, 4, "Number of reader threads");

DEFINE_int64(max_num_write_errors,
             1000,
             "Maximum number of write errors. The test is aborted after this number of errors.");

DEFINE_int64(max_num_read_errors,
             1000,
             "Maximum number of read errors. The test is aborted after this number of errors.");

DEFINE_int32(num_replicas, 3, "Replication factor for the load test table");

DEFINE_int32(num_tablets, 16, "Number of tablets to create in the table");

DEFINE_bool(reads_only, false, "Only read the existing rows from the table.");

DEFINE_bool(writes_only, false, "Writes a new set of rows into an existing table.");

DEFINE_bool(create_table,
            true,
            "Whether the table should be created. Its made false when either "
            "reads_only/writes_only is true. If value is true, existing table will be deleted and "
            "recreated.");

DEFINE_bool(use_kv_table, false, "Use key-value table type backed by RocksDB");

DEFINE_int64(value_size_bytes, 16, "Size of each value in a row being inserted");

DEFINE_int32(retries_on_empty_read,
             0,
             "We can retry up to this many times if we get an empty set of rows on a read "
             "operation");

using strings::Substitute;
using std::atomic_long;
using std::atomic_bool;

using namespace yb::client;
using yb::client::sp::shared_ptr;
using yb::Status;
using yb::ThreadPool;
using yb::ThreadPoolBuilder;
using yb::MonoDelta;
using yb::MemoryOrder;
using yb::ConditionVariable;
using yb::Mutex;
using yb::MutexLock;
using yb::CountDownLatch;
using yb::Slice;
using yb::YBPartialRow;
using yb::TableType;

using strings::Substitute;

using yb::load_generator::KeyIndexSet;
using yb::load_generator::MultiThreadedReader;
using yb::load_generator::MultiThreadedWriter;
using yb::load_generator::SingleThreadedScanner;
using yb::load_generator::FormatHexForLoadTestKey;

// ------------------------------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage(
    "Usage: load_test_tool --master_addresses master1:port1,...,masterN:portN"
  );
  yb::ParseCommandLineFlags(&argc, &argv, true);
  yb::InitGoogleLoggingSafe(argv[0]);

  if (!FLAGS_reads_only)
    LOG(INFO) << "num_keys = " << FLAGS_num_rows;

  for (int i = 0; i < FLAGS_num_iter; ++i) {
    shared_ptr<YBClient> client;
    CHECK_OK(
        YBClientBuilder()
            .add_master_server_addr(FLAGS_load_test_master_addresses)
            .default_rpc_timeout(MonoDelta::FromSeconds(FLAGS_rpc_timeout_sec))
            .Build(&client));

    const string table_name(FLAGS_table_name);

    if (FLAGS_reads_only || FLAGS_writes_only) {
      FLAGS_create_table = false;
    }

    if (FLAGS_reads_only && FLAGS_writes_only) {
      LOG(FATAL) << "Reads only and Writes only options cannot be set together.";
      return 0;
    }

    if (!FLAGS_reads_only && !FLAGS_writes_only && !FLAGS_create_table) {
      LOG(FATAL) << "If reads only or writes only option is not set, then table create should be "
                 << "allowed.";
      return 0;
    }

    LOG(INFO) << "Checking if table '" << table_name << "' already exists";
    {
      YBSchema existing_schema;
      if (client->GetTableSchema(table_name, &existing_schema).ok()) {
        if (FLAGS_create_table) {
          LOG(INFO) << "Table '" << table_name << "' already exists, deleting";
          // Table with the same name already exists, drop it.
          CHECK_OK(client->DeleteTable(table_name));
        }
      } else {
        LOG(INFO) << "Table '" << table_name << "' does not exist yet";

        if (!FLAGS_create_table) {
          LOG(FATAL) << "Exiting as the table was not asked to be created.";
          return 0;
        }
      }
    }

    if (FLAGS_create_table) {
      LOG(INFO) << "Building schema";
      YBSchemaBuilder schemaBuilder;
      schemaBuilder.AddColumn("k")->PrimaryKey()->Type(YBColumnSchema::BINARY)->NotNull();
      schemaBuilder.AddColumn("v")->Type(YBColumnSchema::BINARY)->NotNull();
      YBSchema schema;
      CHECK_OK(schemaBuilder.Build(&schema));

      // Create the number of partitions based on the split keys.
      vector<const YBPartialRow *> splits;
      for (uint64_t j = 1; j < FLAGS_num_tablets; j++) {
        YBPartialRow *row = schema.NewRow();
        // We divide the interval between 0 and 2**64 into the requested number of intervals.
        string split_key = FormatHexForLoadTestKey(
            ((uint64_t) 1 << 62) * 4.0 * j / (FLAGS_num_tablets));
        LOG(INFO) << "split_key #" << j << "=" << split_key;
        CHECK_OK(row->SetBinaryCopy(0, split_key));
        splits.push_back(row);
      }

      LOG(INFO) << "Creating table";

      gscoped_ptr<YBTableCreator> table_creator(client->NewTableCreator());
      Status table_creation_status =
          table_creator->table_name(table_name)
              .schema(&schema)
              .split_rows(splits)
              .num_replicas(FLAGS_num_replicas)
              .table_type(
                  FLAGS_use_kv_table ? YBTableType::KEY_VALUE_TABLE_TYPE
                                     : YBTableType::KUDU_COLUMNAR_TABLE_TYPE)
              .Create();
      if (!table_creation_status.ok()) {
        LOG(INFO) << "Table creation status message: " <<
        table_creation_status.message().ToString();
      }
      if (table_creation_status.message().ToString().find("Table already exists") ==
          std::string::npos) {
        CHECK_OK(table_creation_status);
      }
    }

    shared_ptr<YBTable> table;
    CHECK_OK(client->OpenTable(table_name, &table));

    LOG(INFO) << "Starting load test";
    atomic_bool stop_flag(false);
    if (FLAGS_reads_only) {
      SingleThreadedScanner scanner(table.get());

      scanner.CountRows();
    } else if (FLAGS_writes_only) {
      SingleThreadedScanner scanner(table.get());
      int64_t  existing_num_rows = scanner.CountRows();

      // Adds more keys starting from next index after scanned index
      MultiThreadedWriter writer(
          FLAGS_num_rows + existing_num_rows + 1,
          existing_num_rows + 1,
          FLAGS_num_writer_threads,
          client.get(),
          table.get(),
          &stop_flag,
          FLAGS_value_size_bytes,
          FLAGS_max_num_write_errors);

      writer.Start();
      writer.WaitForCompletion();
    } else {
      MultiThreadedWriter writer(
          FLAGS_num_rows, 0,
          FLAGS_num_writer_threads,
          client.get(),
          table.get(),
          &stop_flag,
          FLAGS_value_size_bytes,
          FLAGS_max_num_write_errors);

      writer.Start();
      MultiThreadedReader reader(
          FLAGS_num_rows,
          FLAGS_num_reader_threads,
          client.get(),
          table.get(),
          writer.InsertionPoint(),
          writer.InsertedKeys(),
          writer.FailedKeys(),
          &stop_flag,
          FLAGS_value_size_bytes,
          FLAGS_max_num_read_errors,
          FLAGS_retries_on_empty_read);

      reader.Start();

      writer.WaitForCompletion();

      // The reader will not stop on its own, so we stop it as soon as the writer stops.
      reader.Stop();
      reader.WaitForCompletion();
    }

    LOG(INFO) << "Test completed (iteration: " << i + 1 << " out of "
              << FLAGS_num_iter << ")";
    LOG(INFO) << string(80, '-');
    LOG(INFO) << "";
  }
  return 0;
}
