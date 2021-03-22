// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/gcs/accessor.h"
#include <future>

namespace ray {
namespace gcs {

Status InternalKVAccessor::InternalKVPut(const std::string &key, const std::string &value,
                                         bool overwrite, bool &added) {
  std::promise<Status> ret_promise;
  AsyncInternalInternalKVPut(
      key, value, overwrite,
      [&ret_promise, &added](Status status, boost::optional<int> added_num) {
        added = static_cast<bool>(added_num.value_or(0));
        ret_promise.set_value(status);
      });
  return ret_promise.get_future().get();
}

Status InternalKVAccessor::InternalKVKeys(const std::string &prefix,
                                          std::vector<std::string> &value) {
  std::promise<Status> ret_promise;
  AsyncInternalKVKeys(prefix, [&ret_promise, &value](Status status, auto &values) {
    value = values.value_or(std::vector<std::string>());
    ret_promise.set_value(status);
  });
  return ret_promise.get_future().get();
}

Status InternalKVAccessor::InternalKVGet(const std::string &key, std::string &value) {
  std::promise<Status> ret_promise;
  AsyncInternalKVGet(key, [&ret_promise, &value](Status status, auto &v) {
    if (v) {
      ret_promise.set_value(status);
      value = *v;
    } else {
      ret_promise.set_value(Status::NotFound("Failed to find the key."));
    }
  });
  return ret_promise.get_future().get();
}

Status InternalKVAccessor::InternalKVDel(const std::string &key) {
  std::promise<Status> ret_promise;
  AsyncInternalKVDel(key,
                     [&ret_promise](Status status) { ret_promise.set_value(status); });
  return ret_promise.get_future().get();
}

Status InternalKVAccessor::InternalKVExists(const std::string &key, bool &exist) {
  std::promise<Status> ret_promise;
  AsyncInternalKVExists(
      key, [&ret_promise, &exist](Status status, const boost::optional<bool> &value) {
        if (value) {
          exist = *value;
        }
        ret_promise.set_value(status);
      });
  return ret_promise.get_future().get();
}

}  // namespace gcs
}  // namespace ray
