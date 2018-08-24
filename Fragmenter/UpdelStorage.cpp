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
#include <algorithm>
#include <boost/variant.hpp>
#include <boost/variant/get.hpp>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "Catalog/Catalog.h"
#include "DataMgr/DataMgr.h"
#include "Fragmenter/InsertOrderFragmenter.h"
#include "Shared/TypedDataAccessors.h"
#include "Shared/thread_count.h"

namespace Fragmenter_Namespace {

void InsertOrderFragmenter::updateColumn(const Catalog_Namespace::Catalog* catalog,
                                         const std::string& tabName,
                                         const std::string& colName,
                                         const int fragmentId,
                                         const std::vector<uint64_t>& fragOffsets,
                                         const std::vector<ScalarTargetValue>& rhsValues,
                                         const SQLTypeInfo& rhsType,
                                         const Data_Namespace::MemoryLevel memoryLevel,
                                         UpdelRoll& updelRoll) {
  const auto td = catalog->getMetadataForTable(tabName);
  CHECK(td);
  const auto cd = catalog->getMetadataForColumn(td->tableId, colName);
  CHECK(cd);
  td->fragmenter->updateColumn(catalog,
                               td,
                               cd,
                               fragmentId,
                               fragOffsets,
                               rhsValues,
                               rhsType,
                               memoryLevel,
                               updelRoll);
}

inline bool is_integral(const SQLTypeInfo& t) {
  return t.is_integer() || t.is_boolean() || t.is_time() || t.is_timeinterval();
}

void InsertOrderFragmenter::updateColumn(const Catalog_Namespace::Catalog* catalog,
                                         const TableDescriptor* td,
                                         const ColumnDescriptor* cd,
                                         const int fragmentId,
                                         const std::vector<uint64_t>& fragOffsets,
                                         const ScalarTargetValue& rhsValue,
                                         const SQLTypeInfo& rhsType,
                                         const Data_Namespace::MemoryLevel memoryLevel,
                                         UpdelRoll& updelRoll) {
  updateColumn(catalog,
               td,
               cd,
               fragmentId,
               fragOffsets,
               std::vector<ScalarTargetValue>(1, rhsValue),
               rhsType,
               memoryLevel,
               updelRoll);
}

void InsertOrderFragmenter::updateColumn(const Catalog_Namespace::Catalog* catalog,
                                         const TableDescriptor* td,
                                         const ColumnDescriptor* cd,
                                         const int fragmentId,
                                         const std::vector<uint64_t>& fragOffsets,
                                         const std::vector<ScalarTargetValue>& rhsValues,
                                         const SQLTypeInfo& rhsType,
                                         const Data_Namespace::MemoryLevel memoryLevel,
                                         UpdelRoll& updelRoll) {
  updelRoll.catalog = catalog;
  updelRoll.logicalTableId = catalog->getLogicalTableId(td->tableId);
  updelRoll.memoryLevel = memoryLevel;

  const auto nrow = fragOffsets.size();
  const auto nval = rhsValues.size();
  if (0 == nrow) {
    return;
  }
  CHECK(nrow == nval || 1 == nval);

  auto fragment_it = std::find_if(
      fragmentInfoVec_.begin(), fragmentInfoVec_.end(), [=](FragmentInfo& f) -> bool {
        return f.fragmentId == fragmentId;
      });
  CHECK(fragment_it != fragmentInfoVec_.end());
  auto& fragment = *fragment_it;
  auto chunk_meta_it = fragment.getChunkMetadataMapPhysical().find(cd->columnId);
  CHECK(chunk_meta_it != fragment.getChunkMetadataMapPhysical().end());
  ChunkKey chunk_key{
      catalog->get_currentDB().dbId, td->tableId, cd->columnId, fragment.fragmentId};
  auto chunk = Chunk_NS::Chunk::getChunk(cd,
                                         &catalog->get_dataMgr(),
                                         chunk_key,
                                         Data_Namespace::CPU_LEVEL,
                                         0,
                                         chunk_meta_it->second.numBytes,
                                         chunk_meta_it->second.numElements);

  const auto ncore = (size_t)cpu_threads();

  std::vector<bool> null(ncore, false);
  std::vector<double> dmax(ncore, std::numeric_limits<double>::min());
  std::vector<double> dmin(ncore, std::numeric_limits<double>::max());
  std::vector<int64_t> lmax(ncore, std::numeric_limits<int64_t>::min());
  std::vector<int64_t> lmin(ncore, std::numeric_limits<int64_t>::max());

  // parallel update elements
  std::vector<std::future<void>> threads;
  std::exception_ptr failed_any_chunk;
  std::mutex mtx;
  auto wait_cleanup_threads = [&] {
    try {
      for (auto& t : threads) {
        t.wait();
      }
      for (auto& t : threads) {
        t.get();
      }
    } catch (...) {
      std::unique_lock<std::mutex> lck(mtx);
      failed_any_chunk = std::current_exception();
    }
    threads.clear();
  };

  const auto segsz = (nrow + ncore - 1) / ncore;
  auto dbuf = chunk->get_buffer();
  auto d0 = dbuf->getMemoryPtr();
  dbuf->setUpdated();
  {
    std::lock_guard<std::mutex> lck(updelRoll.mutex);
    if (updelRoll.dirtyChunks.count(chunk.get()) == 0) {
      updelRoll.dirtyChunks.emplace(chunk.get(), chunk);
    }

    ChunkKey chunkey{updelRoll.catalog->get_currentDB().dbId,
                     cd->tableId,
                     cd->columnId,
                     fragment.fragmentId};
    updelRoll.dirtyChunkeys.insert(chunkey);
  }
  for (size_t rbegin = 0, c = 0; rbegin < nrow; ++c, rbegin += segsz) {
    threads.emplace_back(std::async(
        std::launch::async,
        [=, &null, &lmin, &lmax, &dmin, &dmax, &fragOffsets, &rhsValues] {
      SQLTypeInfo lctype = cd->columnType;

      // !! not sure if this is a undocumented convention or a bug, but for a sharded
      // table the dictionary id of a encoded string column is not specified by
      // comp_param in physical table but somehow in logical table :) comp_param in
      // physical table is always 0, so need to adapt accordingly...
      auto cdl = (shard_ < 0)
                     ? cd
                     : catalog->getMetadataForColumn(
                           catalog->getLogicalTableId(td->tableId), cd->columnId);
      CHECK(cdl);
      StringDictionary* stringDict{nullptr};
      if (lctype.is_string()) {
        CHECK(kENCODING_DICT == lctype.get_compression());
        auto dictDesc = const_cast<DictDescriptor*>(
            catalog->getMetadataForDict(cdl->columnType.get_comp_param()));
        CHECK(dictDesc);
        stringDict = dictDesc->stringDict.get();
        CHECK(stringDict);
      }

      for (size_t r = rbegin; r < std::min(rbegin + segsz, nrow); r++) {
        const auto roffs = fragOffsets[r];
        auto dptr = d0 + roffs * get_element_size(lctype);
        auto sv = &rhsValues[1 == nval ? 0 : r];
        ScalarTargetValue sv2;

        // Subtle here is on the two cases of string-to-string assignments, when
        // upstream passes RHS string as a string index instead of a preferred "real
        // string".
        //   case #1. For "SET str_col = str_literal", it is hard to resolve temp str
        //   index
        //            in this layer, so if upstream passes a str idx here, an
        //            exception is thrown.
        //   case #2. For "SET str_col1 = str_col2", RHS str idx is converted to LHS
        //   str idx.
        if (rhsType.is_string()) {
          if (const auto vp = boost::get<int64_t>(sv)) {
            auto dictDesc = const_cast<DictDescriptor*>(
                catalog->getMetadataForDict(rhsType.get_comp_param()));
            if (nullptr == dictDesc) {
              throw std::runtime_error(
                  "UPDATE does not support cast from string literal to string "
                  "column.");
            }
            auto stringDict = dictDesc->stringDict.get();
            CHECK(stringDict);
            sv2 = NullableString(stringDict->getString(*vp));
            sv = &sv2;
          }
        }

        if (const auto vp = boost::get<int64_t>(sv)) {
          auto v = *vp;
          if (lctype.is_string()) {
#ifdef ENABLE_STRING_CONVERSION_AT_STORAGE_LAYER
            v = stringDict->getOrAdd(DatumToString(
                rhsType.is_time() ? Datum{.timeval = v} : Datum{.bigintval = v},
                rhsType));
#else
                    throw std::runtime_error("UPDATE does not support cast to string.");
                  }
#endif
            put_scalar<int64_t>(dptr, lctype, v, cd->columnName, &rhsType);
            if (lctype.is_decimal()) {
              int64_t decimal;
              get_scalar<int64_t>(dptr, lctype, decimal);
              set_minmax<int64_t>(lmin[c], lmax[c], decimal);
              if (!((v >= 0) ^ (decimal < 0))) {
                throw std::runtime_error("Data conversion overflow on " +
                                         std::to_string(v) + " from DECIMAL(" +
                                         std::to_string(rhsType.get_dimension()) + ", " +
                                         std::to_string(rhsType.get_scale()) + ") to (" +
                                         std::to_string(lctype.get_dimension()) + ", " +
                                         std::to_string(lctype.get_scale()) + ")");
              }
            } else if (is_integral(lctype)) {
              set_minmax<int64_t>(
                  lmin[c],
                  lmax[c],
                  rhsType.is_decimal() ? round(decimal_to_double(rhsType, v)) : v);
            } else {
              set_minmax<double>(
                  dmin[c],
                  dmax[c],
                  rhsType.is_decimal() ? decimal_to_double(rhsType, v) : v);
            }
          } else if (const auto vp = boost::get<double>(sv)) {
            auto v = *vp;
            if (lctype.is_string()) {
#ifdef ENABLE_STRING_CONVERSION_AT_STORAGE_LAYER
              v = stringDict->getOrAdd(DatumToString(Datum{.doubleval = v}, rhsType));
#else
                    throw std::runtime_error("UPDATE does not support cast to string.");
                  }
#endif
              put_scalar<double>(dptr, lctype, v, cd->columnName);
              if (lctype.is_integer()) {
                set_minmax<int64_t>(lmin[c], lmax[c], v);
              } else {
                set_minmax<double>(dmin[c], dmax[c], v);
              }
            } else if (const auto vp = boost::get<float>(sv)) {
              auto v = *vp;
              if (lctype.is_string()) {
#ifdef ENABLE_STRING_CONVERSION_AT_STORAGE_LAYER
                v = stringDict->getOrAdd(DatumToString(Datum{.floatval = v}, rhsType));
#else
                    throw std::runtime_error("UPDATE does not support cast to string.");
                  }
#endif
                put_scalar<float>(dptr, lctype, v, cd->columnName);
                if (lctype.is_integer()) {
                  set_minmax<int64_t>(lmin[c], lmax[c], v);
                } else {
                  set_minmax<double>(dmin[c], dmax[c], v);
                }
              } else if (const auto vp = boost::get<NullableString>(sv)) {
                const auto s = boost::get<std::string>(vp);
                const auto sval = s ? *s : std::string("");
                if (lctype.is_string()) {
                  decltype(stringDict->getOrAdd(sval)) sidx;
                  {
                    std::unique_lock<std::mutex> lock(temp_mutex_);
                    sidx = stringDict->getOrAdd(sval);
                  }
                  put_scalar<int32_t>(dptr, lctype, sidx, cd->columnName);
                  set_minmax<int64_t>(lmin[c], lmax[c], sidx);
                } else if (sval.size() > 0) {
                  auto dval = std::atof(sval.data());
                  if (lctype.is_boolean()) {
                    dval = sval == "t" || sval == "true" || sval == "T" || sval == "True";
                  } else if (lctype.is_time()) {
                    dval = StringToDatum(sval, lctype).timeval;
                  }
                  if (lctype.is_fp() || lctype.is_decimal()) {
                    put_scalar<double>(dptr, lctype, dval, cd->columnName);
                    set_minmax<double>(dmin[c], dmax[c], dval);
                  } else {
                    put_scalar<int64_t>(dptr, lctype, dval, cd->columnName);
                    set_minmax<int64_t>(lmin[c], lmax[c], dval);
                  }
                } else {
                  put_null(dptr, lctype, cd->columnName);
                  null[c] = true;
                }
              } else {
                CHECK(false);
              }
            }
          }));
          if (threads.size() >= (size_t)cpu_threads()) {
            wait_cleanup_threads();
          }
          if (failed_any_chunk) {
            break;
          }
        }
        wait_cleanup_threads();
        if (failed_any_chunk) {
          std::rethrow_exception(failed_any_chunk);
        }

        bool lc_null{false};
        double lc_dmax{std::numeric_limits<double>::min()};
        double lc_dmin{std::numeric_limits<double>::max()};
        int64_t lc_lmax{std::numeric_limits<int64_t>::min()};
        int64_t lc_lmin{std::numeric_limits<int64_t>::max()};
        for (size_t c = 0; c < ncore; ++c) {
          lc_null |= null[c];
          lc_dmax = std::max<double>(lc_dmax, dmax[c]);
          lc_dmin = std::min<double>(lc_dmin, dmin[c]);
          lc_lmax = std::max<int64_t>(lc_lmax, lmax[c]);
          lc_lmin = std::min<int64_t>(lc_lmin, lmin[c]);
        }
        updateColumnMetadata(cd,
                             fragment,
                             chunk,
                             lc_null,
                             lc_dmax,
                             lc_dmin,
                             lc_lmax,
                             lc_lmin,
                             cd->columnType,
                             updelRoll);
      }

      void InsertOrderFragmenter::updateColumnMetadata(
          const ColumnDescriptor* cd,
          FragmentInfo& fragment,
          std::shared_ptr<Chunk_NS::Chunk> chunk,
          const bool null,
          const double dmax,
          const double dmin,
          const int64_t lmax,
          const int64_t lmin,
          const SQLTypeInfo& rhsType,
          UpdelRoll& updelRoll) {
        auto td = updelRoll.catalog->getMetadataForTable(cd->tableId);
        auto key = std::make_pair(td, &fragment);
        std::lock_guard<std::mutex> lck(updelRoll.mutex);
        if (0 == updelRoll.chunkMetadata.count(key)) {
          updelRoll.chunkMetadata[key] = fragment.getChunkMetadataMapPhysical();
        }
        if (0 == updelRoll.numTuples.count(key)) {
          updelRoll.numTuples[key] = fragment.shadowNumTuples;
        }
        auto& chunkMetadata = updelRoll.chunkMetadata[key];

        auto buffer = chunk->get_buffer();
        const auto& lctype = cd->columnType;
        if (is_integral(lctype) || (lctype.is_decimal() && rhsType.is_decimal())) {
          buffer->encoder->updateStats(lmax, null);
          buffer->encoder->updateStats(lmin, null);
        } else if (lctype.is_fp()) {
          buffer->encoder->updateStats(dmax, null);
          buffer->encoder->updateStats(dmin, null);
        } else if (lctype.is_decimal()) {
          buffer->encoder->updateStats((int64_t)(dmax * pow(10, lctype.get_scale())),
                                       null);
          buffer->encoder->updateStats((int64_t)(dmin * pow(10, lctype.get_scale())),
                                       null);
        } else if (!lctype.is_array() &&
                   !(lctype.is_string() && kENCODING_DICT != lctype.get_compression())) {
          buffer->encoder->updateStats(lmax, null);
          buffer->encoder->updateStats(lmin, null);
        }
        buffer->encoder->getMetadata(chunkMetadata[cd->columnId]);

        // removed as @alex suggests. keep it commented in case of any chance to revisit
        // it once after vacuum code is introduced. fragment.invalidateChunkMetadataMap();
      }

      void InsertOrderFragmenter::updateMetadata(
          const Catalog_Namespace::Catalog* catalog,
          const MetaDataKey& key,
          UpdelRoll& updelRoll) {
        mapd_unique_lock<mapd_shared_mutex> writeLock(fragmentInfoMutex_);
        if (updelRoll.chunkMetadata.count(key)) {
          auto& fragmentInfo = *key.second;
          const auto& chunkMetadata = updelRoll.chunkMetadata[key];
          fragmentInfo.shadowChunkMetadataMap = chunkMetadata;
          fragmentInfo.setChunkMetadataMap(chunkMetadata);
          fragmentInfo.shadowNumTuples = updelRoll.numTuples[key];
          fragmentInfo.setPhysicalNumTuples(fragmentInfo.shadowNumTuples);
          // TODO(ppan): When fragment-level compaction is enable, the following code
          // should suffice. When not (ie. existing code), we'll revert to update
          // InsertOrderFragmenter::varLenColInfo_
          /*
          for (const auto cit : chunkMetadata) {
            const auto& cd = *catalog->getMetadataForColumn(td->tableId, cit.first);
            if (cd.columnType.get_size() < 0)
              fragmentInfo.varLenColInfox[cd.columnId] = cit.second.numBytes;
          }
          */
        }
      }

}  // namespace Fragmenter_Namespace

void UpdelRoll::commitUpdate() {
      if (nullptr == catalog) {
        return;
      }
      const auto td = catalog->getMetadataForTable(logicalTableId);
      CHECK(td);
      // checkpoint all shards regardless, or epoch becomes out of sync
      if (td->persistenceLevel == Data_Namespace::MemoryLevel::DISK_LEVEL) {
        catalog->checkpoint(logicalTableId);
      }
      // for each dirty fragment
      for (auto& cm : chunkMetadata) {
        cm.first.first->fragmenter->updateMetadata(catalog, cm.first, *this);
      }
      dirtyChunks.clear();
      // flush gpu dirty chunks if update was not on gpu
      if (memoryLevel != Data_Namespace::MemoryLevel::GPU_LEVEL) {
        for (const auto& chunkey : dirtyChunkeys) {
          catalog->get_dataMgr().deleteChunksWithPrefix(
              chunkey, Data_Namespace::MemoryLevel::GPU_LEVEL);
        }
      }
}

void UpdelRoll::cancelUpdate() {
      if (nullptr == catalog) {
        return;
      }
      const auto td = catalog->getMetadataForTable(logicalTableId);
      CHECK(td);
      if (td->persistenceLevel != memoryLevel) {
        for (auto dit : dirtyChunks) {
          catalog->get_dataMgr().free(dit.first->get_buffer());
          dit.first->set_buffer(nullptr);
        }
      }
}
