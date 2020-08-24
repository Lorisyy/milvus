// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#pragma once

#include "server/delivery/request/BaseReq.h"
#include "utils/BlockingQueue.h"
#include "utils/Status.h"

#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace milvus {
namespace server {

class ReqStrategy {
 protected:
    ReqStrategy() = default;

 public:
    virtual Status
    ReScheduleQueue(const BaseReqPtr& req, std::queue<BaseReqPtr>& queue) = 0;
};

using ReqStrategyPtr = std::shared_ptr<ReqStrategy>;

}  // namespace server
}  // namespace milvus
