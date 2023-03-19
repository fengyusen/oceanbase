// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   suzhi.yt <>

#pragma once

#include "lib/lock/ob_mutex.h"
#include "share/table/ob_table_load_handle.h"
#include "storage/direct_load/ob_direct_load_mem_context.h"
#include "storage/direct_load/ob_direct_load_mem_define.h"
#include "storage/direct_load/ob_direct_load_multi_map.h"
#include "storage/direct_load/ob_direct_load_sstable_builder.h"

namespace oceanbase
{
namespace storage
{
class ObIDirectLoadPartitionTable;

class ObDirectLoadMemDump
{
  typedef ObDirectLoadConstExternalMultiPartitionRow RowType;
  typedef ObDirectLoadExternalMultiPartitionRowChunk ChunkType;
  typedef ObDirectLoadExternalMultiPartitionRowRange RangeType;
  typedef ObDirectLoadExternalMultiPartitionRowCompare CompareType;
public:
  class Context
  {
  public:
    Context();
    ~Context();
    int add_table(const common::ObTabletID &tablet_id, int64_t range_idx,
                  ObIDirectLoadPartitionTable *table);

    int init()
    {
      return tables_.init();
    }

  private:
    ObArenaAllocator allocator_; // just for safe_allocator_
  public:
    ObSafeArenaAllocator safe_allocator_;
    //注意，如果这个tables_会被多线程操作，必须加锁
    ObDirectLoadMultiMap<common::ObTabletID, std::pair<int64_t, ObIDirectLoadPartitionTable *>>
      tables_;
    common::ObArray<ChunkType *> mem_chunk_array_;
    int64_t finished_sub_dump_count_;
    int64_t sub_dump_count_;

  private:
    lib::ObMutex mutex_;
    ObArray<ObIDirectLoadPartitionTable *> all_tables_;
  };

public:
  ObDirectLoadMemDump(ObDirectLoadMemContext *mem_ctx,
                      const RangeType &range,
                      table::ObTableLoadHandle<Context> context_ptr, int64_t range_idx);
  ~ObDirectLoadMemDump();
  int do_dump();

private:
  // dump tables
  int new_table_builder(const ObTabletID &tablet_id,
                        ObIDirectLoadPartitionTableBuilder *&table_builder);
  int new_sstable_builder(const ObTabletID &tablet_id,
                          ObIDirectLoadPartitionTableBuilder *&table_builder);
  int new_external_table_builder(const ObTabletID &tablet_id,
                                 ObIDirectLoadPartitionTableBuilder *&table_builder);
  int close_table_builder(ObIDirectLoadPartitionTableBuilder *table_builder,
                          common::ObTabletID tablet_id, bool is_final);
  int dump_tables();
  // compact tables
  int new_table_compactor(const common::ObTabletID &tablet_id,
                          ObIDirectLoadTabletTableCompactor *&compactor);
  int new_sstable_compactor(const common::ObTabletID &tablet_id,
                            ObIDirectLoadTabletTableCompactor *&compactor);
  int new_external_table_compactor(const common::ObTabletID &tablet_id,
                                   ObIDirectLoadTabletTableCompactor *&compactor);
  int compact_tables();
  int compact_tablet_tables(const common::ObTabletID &tablet_id);

private:
  // data members
  ObArenaAllocator allocator_;
  ObDirectLoadMemContext *mem_ctx_;
  RangeType range_;
  table::ObTableLoadHandle<Context> context_ptr_;
  int64_t range_idx_;
  char *extra_buf_;
  int64_t extra_buf_size_;
};

} // namespace storage
} // namespace oceanbase
