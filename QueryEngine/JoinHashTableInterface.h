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
#ifndef QUERYENGINE_JOINHASHTABLEINTERFACE_H
#define QUERYENGINE_JOINHASHTABLEINTERFACE_H

#include <llvm/IR/Value.h>
#include <cstdint>
#include "CompilationOptions.h"

class TooManyHashEntries : public std::runtime_error {
 public:
  TooManyHashEntries()
      : std::runtime_error("Hash tables with more than 2B entries not supported yet") {}
};

class HashJoinFail : public std::runtime_error {
 public:
  HashJoinFail(const std::string& reason) : std::runtime_error(reason) {}
};

class NeedsOneToManyHash : public HashJoinFail {
 public:
  NeedsOneToManyHash() : HashJoinFail("Needs one to many hash") {}
};

class FailedToFetchColumn : public HashJoinFail {
 public:
  FailedToFetchColumn()
      : HashJoinFail("Not enough memory for columns involvde in join") {}
};

class FailedToJoinOnVirtualColumn : public HashJoinFail {
 public:
  FailedToJoinOnVirtualColumn() : HashJoinFail("Cannot join on rowid") {}
};

#ifndef ENABLE_MULTIFRAG_JOIN
class MultiFragJoinNotSupported : public HashJoinFail {
 public:
  MultiFragJoinNotSupported()
      : HashJoinFail("Muli-fragment inner table not supported yet") {}

  MultiFragJoinNotSupported(const std::string& table_name)
      : HashJoinFail(std::string("Muli-fragment inner table '") + table_name +
                     std::string("' not supported yet")) {}
};
#endif

struct HashJoinMatchingSet {
  llvm::Value* elements;
  llvm::Value* count;
  llvm::Value* slot;
};

class JoinHashTableInterface {
 public:
  virtual int64_t getJoinHashBuffer(const ExecutorDeviceType device_type,
                                    const int device_id) noexcept = 0;

  virtual llvm::Value* codegenSlotIsValid(const CompilationOptions&, const size_t) = 0;

  virtual llvm::Value* codegenSlot(const CompilationOptions&, const size_t) = 0;

  virtual HashJoinMatchingSet codegenMatchingSet(const CompilationOptions&,
                                                 const size_t) = 0;

  virtual int getInnerTableId() const noexcept = 0;

  virtual int getInnerTableRteIdx() const noexcept = 0;

  enum class HashType {
    OneToOne,
    OneToMany,
  };

  virtual HashType getHashType() const noexcept = 0;
};

#endif  // QUERYENGINE_JOINHASHTABLEINTERFACE_H
