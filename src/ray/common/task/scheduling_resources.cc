// Copyright 2019-2021 The Ray Authors.
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

#include "ray/common/task/scheduling_resources.h"

#include <cmath>
#include <sstream>

#include "absl/container/flat_hash_map.h"
#include "ray/util/logging.h"

namespace ray {
ResourceSet::ResourceSet() {}

ResourceSet::ResourceSet(const std::unordered_map<std::string, FixedPoint> &resource_map)
    : resource_capacity_(resource_map) {
  for (auto const &resource_pair : resource_map) {
    RAY_CHECK(resource_pair.second > 0);
  }
}

ResourceSet::ResourceSet(const std::unordered_map<std::string, double> &resource_map) {
  for (auto const &resource_pair : resource_map) {
    RAY_CHECK(resource_pair.second > 0);
    resource_capacity_[resource_pair.first] = FixedPoint(resource_pair.second);
  }
}

ResourceSet::ResourceSet(const std::vector<std::string> &resource_labels,
                         const std::vector<double> resource_capacity) {
  RAY_CHECK(resource_labels.size() == resource_capacity.size());
  for (size_t i = 0; i < resource_labels.size(); i++) {
    RAY_CHECK(resource_capacity[i] > 0);
    resource_capacity_[resource_labels[i]] = FixedPoint(resource_capacity[i]);
  }
}

ResourceSet::~ResourceSet() {}

bool ResourceSet::operator==(const ResourceSet &rhs) const {
  return (this->IsSubset(rhs) && rhs.IsSubset(*this));
}

bool ResourceSet::IsEmpty() const {
  // Check whether the capacity of each resource type is zero. Exit early if not.
  return resource_capacity_.empty();
}

bool ResourceSet::IsSubset(const ResourceSet &other) const {
  // Check to make sure all keys of this are in other.
  for (const auto &resource_pair : resource_capacity_) {
    const auto &resource_name = resource_pair.first;
    const FixedPoint &lhs_quantity = resource_pair.second;
    const FixedPoint &rhs_quantity = other.GetResource(resource_name);
    if (lhs_quantity > rhs_quantity) {
      // Resource found in rhs, but lhs capacity exceeds rhs capacity.
      return false;
    }
  }
  return true;
}

/// Test whether this ResourceSet is a superset of the other ResourceSet
bool ResourceSet::IsSuperset(const ResourceSet &other) const {
  return other.IsSubset(*this);
}

/// Test whether this ResourceSet is precisely equal to the other ResourceSet.
bool ResourceSet::IsEqual(const ResourceSet &rhs) const {
  return (this->IsSubset(rhs) && rhs.IsSubset(*this));
}

void ResourceSet::AddOrUpdateResource(const std::string &resource_name,
                                      const FixedPoint &capacity) {
  if (capacity > 0) {
    resource_capacity_[resource_name] = capacity;
  }
}

bool ResourceSet::DeleteResource(const std::string &resource_name) {
  if (resource_capacity_.count(resource_name) == 1) {
    resource_capacity_.erase(resource_name);
    return true;
  } else {
    return false;
  }
}

void ResourceSet::SubtractResources(const ResourceSet &other) {
  // Subtract the resources, make sure none goes below zero and delete any if new capacity
  // is zero.
  for (const auto &resource_pair : other.GetResourceAmountMap()) {
    const std::string &resource_label = resource_pair.first;
    const FixedPoint &resource_capacity = resource_pair.second;
    if (resource_capacity_.count(resource_label) == 1) {
      resource_capacity_[resource_label] -= resource_capacity;
    }
    if (resource_capacity_[resource_label] <= 0) {
      resource_capacity_.erase(resource_label);
    }
  }
}

void ResourceSet::SubtractResourcesStrict(const ResourceSet &other) {
  // Subtract the resources, make sure none goes below zero and delete any if new capacity
  // is zero.
  for (const auto &resource_pair : other.GetResourceAmountMap()) {
    const std::string &resource_label = resource_pair.first;
    const FixedPoint &resource_capacity = resource_pair.second;
    RAY_CHECK(resource_capacity_.count(resource_label) == 1)
        << "Attempt to acquire unknown resource: " << resource_label << " capacity "
        << resource_capacity.Double();
    resource_capacity_[resource_label] -= resource_capacity;

    // Ensure that quantity is positive. Note, we have to have the check before
    // erasing the object to make sure that it doesn't get added back.
    RAY_CHECK(resource_capacity_[resource_label] >= 0)
        << "Capacity of resource after subtraction is negative, "
        << resource_capacity_[resource_label].Double() << ".";

    if (resource_capacity_[resource_label] == 0) {
      resource_capacity_.erase(resource_label);
    }
  }
}

// Add a set of resources to the current set of resources subject to upper limits on
// capacity from the total_resource set
void ResourceSet::AddResourcesCapacityConstrained(const ResourceSet &other,
                                                  const ResourceSet &total_resources) {
  const std::unordered_map<std::string, FixedPoint> &total_resource_map =
      total_resources.GetResourceAmountMap();
  for (const auto &resource_pair : other.GetResourceAmountMap()) {
    const std::string &to_add_resource_label = resource_pair.first;
    const FixedPoint &to_add_resource_capacity = resource_pair.second;
    if (total_resource_map.count(to_add_resource_label) != 0) {
      // If resource exists in total map, add to the local capacity map.
      // If the new capacity is less than the total capacity, set the new capacity to
      // the local capacity (capping to the total).
      const FixedPoint &total_capacity = total_resource_map.at(to_add_resource_label);
      resource_capacity_[to_add_resource_label] =
          std::min(resource_capacity_[to_add_resource_label] + to_add_resource_capacity,
                   total_capacity);
    } else {
      // Resource does not exist in the total map, it probably got deleted from the total.
      // Don't panic, do nothing and simply continue.
      RAY_LOG(DEBUG) << "[AddResourcesCapacityConstrained] Resource "
                     << to_add_resource_label
                     << " not found in the total resource map. It probably got deleted, "
                        "not adding back to resource_capacity_.";
    }
  }
}

// Perform an outer join.
void ResourceSet::AddResources(const ResourceSet &other) {
  for (const auto &resource_pair : other.GetResourceAmountMap()) {
    const std::string &resource_label = resource_pair.first;
    const FixedPoint &resource_capacity = resource_pair.second;
    resource_capacity_[resource_label] += resource_capacity;
  }
}

FixedPoint ResourceSet::GetResource(const std::string &resource_name) const {
  if (resource_capacity_.count(resource_name) == 0) {
    return 0;
  }
  const FixedPoint &capacity = resource_capacity_.at(resource_name);
  return capacity;
}

const ResourceSet ResourceSet::GetNumCpus() const {
  ResourceSet cpu_resource_set;
  const FixedPoint cpu_quantity = GetResource(kCPU_ResourceLabel);
  if (cpu_quantity > 0) {
    cpu_resource_set.resource_capacity_[kCPU_ResourceLabel] = cpu_quantity;
  }
  return cpu_resource_set;
}

std::string format_resource(std::string resource_name, double quantity) {
  if (resource_name == "object_store_memory" ||
      resource_name.find(kMemory_ResourceLabel) == 0) {
    // The memory resources (in 50MiB unit) are converted to GiB
    return std::to_string(quantity * 50 / 1024) + " GiB";
  }
  return std::to_string(quantity);
}

const std::string ResourceSet::ToString() const {
  if (resource_capacity_.size() == 0) {
    return "{}";
  } else {
    std::string return_string = "";

    auto it = resource_capacity_.begin();

    // Convert the first element to a string.
    if (it != resource_capacity_.end()) {
      double resource_amount = (it->second).Double();
      return_string +=
          "{" + it->first + ": " + format_resource(it->first, resource_amount) + "}";
      it++;
    }

    // Add the remaining elements to the string (along with a comma).
    for (; it != resource_capacity_.end(); ++it) {
      double resource_amount = (it->second).Double();
      return_string +=
          ", {" + it->first + ": " + format_resource(it->first, resource_amount) + "}";
    }

    return return_string;
  }
}

const std::unordered_map<std::string, double> ResourceSet::GetResourceMap() const {
  std::unordered_map<std::string, double> result;
  for (const auto &resource_pair : resource_capacity_) {
    result[resource_pair.first] = resource_pair.second.Double();
  }
  return result;
};

const std::unordered_map<std::string, FixedPoint> &ResourceSet::GetResourceAmountMap()
    const {
  return resource_capacity_;
};

/// ResourceIds class implementation

ResourceIds::ResourceIds() {}

ResourceIds::ResourceIds(double resource_quantity) {
  RAY_CHECK(IsWhole(resource_quantity));
  int64_t whole_quantity = resource_quantity;
  whole_ids_.reserve(whole_quantity);
  for (int64_t i = 0; i < whole_quantity; ++i) {
    whole_ids_.push_back(i);
  }
  total_capacity_ = TotalQuantity();
  decrement_backlog_ = 0;
}

ResourceIds::ResourceIds(const std::vector<int64_t> &whole_ids)
    : whole_ids_(whole_ids),
      total_capacity_((uint64_t)whole_ids.size()),
      decrement_backlog_(0) {}

ResourceIds::ResourceIds(
    const std::vector<std::pair<int64_t, FixedPoint>> &fractional_ids)
    : fractional_ids_(fractional_ids),
      total_capacity_(TotalQuantity()),
      decrement_backlog_(0) {}

ResourceIds::ResourceIds(
    const std::vector<int64_t> &whole_ids,
    const std::vector<std::pair<int64_t, FixedPoint>> &fractional_ids)
    : whole_ids_(whole_ids),
      fractional_ids_(fractional_ids),
      total_capacity_(TotalQuantity()),
      decrement_backlog_(0) {}

bool ResourceIds::Contains(const FixedPoint &resource_quantity) const {
  if (resource_quantity >= 1) {
    double whole_quantity = resource_quantity.Double();
    RAY_CHECK(IsWhole(whole_quantity));
    return whole_ids_.size() >= whole_quantity;
  } else {
    if (whole_ids_.size() > 0) {
      return true;
    } else {
      for (auto const &fractional_pair : fractional_ids_) {
        if (fractional_pair.second >= resource_quantity) {
          return true;
        }
      }
      return false;
    }
  }
}

ResourceIds ResourceIds::Acquire(const FixedPoint &resource_quantity) {
  if (resource_quantity >= 1) {
    // Handle the whole case.
    double whole_quantity = resource_quantity.Double();
    RAY_CHECK(IsWhole(whole_quantity));
    RAY_CHECK(static_cast<int64_t>(whole_ids_.size()) >=
              static_cast<int64_t>(whole_quantity));

    std::vector<int64_t> ids_to_return;
    for (int64_t i = 0; i < whole_quantity; ++i) {
      ids_to_return.push_back(whole_ids_.back());
      whole_ids_.pop_back();
    }

    return ResourceIds(ids_to_return);

  } else {
    // Handle the fractional case.
    for (auto &fractional_pair : fractional_ids_) {
      if (fractional_pair.second >= resource_quantity) {
        auto return_pair = std::make_pair(fractional_pair.first, resource_quantity);
        fractional_pair.second -= resource_quantity;

        // Remove the fractional pair if the new capacity is 0
        if (fractional_pair.second == 0) {
          std::swap(fractional_pair, fractional_ids_[fractional_ids_.size() - 1]);
          fractional_ids_.pop_back();
        }
        return ResourceIds({return_pair});
      }
    }

    // If we get here then there weren't enough available fractional IDs, so we
    // need to use a whole ID.
    RAY_CHECK(whole_ids_.size() > 0);
    int64_t whole_id = whole_ids_.back();
    whole_ids_.pop_back();

    auto return_pair = std::make_pair(whole_id, resource_quantity);
    // We cannot make use of the implicit conversion because ints have no
    // operator-(const FixedPoint&) function.
    const FixedPoint remaining_amount = FixedPoint(1) - resource_quantity;
    fractional_ids_.push_back(std::make_pair(whole_id, remaining_amount));
    return ResourceIds({return_pair});
  }
}

void ResourceIds::Release(const ResourceIds &resource_ids) {
  auto const &whole_ids_to_return = resource_ids.WholeIds();

  int64_t return_resource_count = whole_ids_to_return.size();
  if (return_resource_count > decrement_backlog_) {
    // We are returning more resources than in the decrement backlog, thus set the backlog
    // to zero and insert (count - decrement_backlog resources).
    whole_ids_.insert(whole_ids_.end(), whole_ids_to_return.begin() + decrement_backlog_,
                      whole_ids_to_return.end());
    decrement_backlog_ = 0;
  } else {
    // Do not insert back to whole_ids_. Instead just decrement backlog by the return
    // count
    decrement_backlog_ -= return_resource_count;
  }

  // Return the fractional IDs.
  auto const &fractional_ids_to_return = resource_ids.FractionalIds();
  for (auto const &fractional_pair_to_return : fractional_ids_to_return) {
    int64_t resource_id = fractional_pair_to_return.first;
    auto const &fractional_pair_it =
        std::find_if(fractional_ids_.begin(), fractional_ids_.end(),
                     [resource_id](std::pair<int64_t, FixedPoint> &fractional_pair) {
                       return fractional_pair.first == resource_id;
                     });
    if (fractional_pair_it == fractional_ids_.end()) {
      fractional_ids_.push_back(fractional_pair_to_return);
    } else {
      fractional_pair_it->second += fractional_pair_to_return.second;
      RAY_CHECK(fractional_pair_it->second <= 1)
          << "Fractional Resource Id " << fractional_pair_it->first << " capacity is "
          << fractional_pair_it->second.Double() << ". Should have been less than one.";
      // If this makes the ID whole, then return it to the list of whole IDs.
      if (fractional_pair_it->second == 1) {
        if (decrement_backlog_ > 0) {
          // There's a decrement backlog, do not add to whole_ids_
          decrement_backlog_--;
        } else {
          whole_ids_.push_back(resource_id);
        }
        fractional_ids_.erase(fractional_pair_it);
      }
    }
  }
}

ResourceIds ResourceIds::Plus(const ResourceIds &resource_ids) const {
  ResourceIds resource_ids_to_return(whole_ids_, fractional_ids_);
  resource_ids_to_return.Release(resource_ids);
  return resource_ids_to_return;
}

const std::vector<int64_t> &ResourceIds::WholeIds() const { return whole_ids_; }

const std::vector<std::pair<int64_t, FixedPoint>> &ResourceIds::FractionalIds() const {
  return fractional_ids_;
}

bool ResourceIds::TotalQuantityIsZero() const {
  return whole_ids_.empty() && fractional_ids_.empty();
}

FixedPoint ResourceIds::TotalQuantity() const {
  FixedPoint total_quantity = FixedPoint((uint64_t)whole_ids_.size());
  for (auto const &fractional_pair : fractional_ids_) {
    total_quantity += fractional_pair.second;
  }
  return total_quantity;
}

std::string ResourceIds::ToString() const {
  std::string return_string = "Whole IDs: [";
  for (auto const &whole_id : whole_ids_) {
    return_string += std::to_string(whole_id) + ", ";
  }
  return_string += "], Fractional IDs: ";
  for (auto const &fractional_pair : fractional_ids_) {
    double fractional_amount = fractional_pair.second.Double();
    return_string += "(" + std::to_string(fractional_pair.first) + ", " +
                     std::to_string(fractional_amount) + "), ";
  }
  return_string += "]";
  return return_string;
}

void ResourceIds::UpdateCapacity(int64_t new_capacity) {
  // Assert the new capacity is positive for sanity
  RAY_CHECK(new_capacity >= 0);
  int64_t capacity_delta = new_capacity - total_capacity_.Double();
  if (capacity_delta < 0) {
    DecreaseCapacity(-1 * capacity_delta);
  } else {
    IncreaseCapacity(capacity_delta);
  }
}

void ResourceIds::IncreaseCapacity(int64_t increment_quantity) {
  // Adjust with decrement_backlog_
  int64_t actual_increment_quantity = 0;
  actual_increment_quantity =
      std::max<int64_t>(0, increment_quantity - decrement_backlog_);
  decrement_backlog_ = std::max<int64_t>(0, decrement_backlog_ - increment_quantity);

  if (actual_increment_quantity > 0) {
    for (int i = 0; i < actual_increment_quantity; i++) {
      whole_ids_.push_back(-1);  // Dynamic resources are assigned resource id -1.
    }
    total_capacity_ += actual_increment_quantity;
  }
}

void ResourceIds::DecreaseCapacity(int64_t decrement_quantity) {
  // Get total quantity, but casting to int to truncate any fractional resources. Updates
  // are supported only on whole resources.
  int64_t available_quantity = TotalQuantity().Double();
  RAY_LOG(DEBUG) << "[DecreaseCapacity] Available quantity: " << available_quantity;

  if (available_quantity < decrement_quantity) {
    RAY_LOG(DEBUG) << "[DecreaseCapacity] Available quantity < decrement quantity  "
                   << decrement_quantity;
    // We're trying to remove more resources than are available
    // In this case, add the difference to the decrement backlog, and when resources are
    // released the backlog will be cleared
    decrement_backlog_ += (decrement_quantity - available_quantity);
    // To decrease capacity, just acquire resources and forget about them. They are popped
    // from whole_ids when acquired.
    Acquire(available_quantity);
  } else {
    RAY_LOG(DEBUG) << "[DecreaseCapacity] Available quantity > decrement quantity  "
                   << decrement_quantity;
    // Simply acquire resources if sufficient are available
    Acquire(decrement_quantity);
  }
  total_capacity_ -= decrement_quantity;
}

bool ResourceIds::IsWhole(double resource_quantity) const {
  int64_t whole_quantity = resource_quantity;
  return whole_quantity == resource_quantity;
}

/// ResourceIdSet class implementation

ResourceIdSet::ResourceIdSet() {}

ResourceIdSet::ResourceIdSet(const ResourceSet &resource_set) {
  for (auto const &resource_pair : resource_set.GetResourceMap()) {
    auto const &resource_name = resource_pair.first;
    double resource_quantity = resource_pair.second;
    available_resources_[resource_name] = ResourceIds(resource_quantity);
  }
}

ResourceIdSet::ResourceIdSet(
    const std::unordered_map<std::string, ResourceIds> &available_resources)
    : available_resources_(available_resources) {}

bool ResourceIdSet::Contains(const ResourceSet &resource_set) const {
  for (auto const &resource_pair : resource_set.GetResourceAmountMap()) {
    auto const &resource_name = resource_pair.first;
    const FixedPoint &resource_quantity = resource_pair.second;

    auto it = available_resources_.find(resource_name);
    if (it == available_resources_.end()) {
      return false;
    }

    if (!it->second.Contains(resource_quantity)) {
      return false;
    }
  }
  return true;
}

ResourceIdSet ResourceIdSet::Acquire(const ResourceSet &resource_set) {
  std::unordered_map<std::string, ResourceIds> acquired_resources;

  for (auto const &resource_pair : resource_set.GetResourceAmountMap()) {
    auto const &resource_name = resource_pair.first;
    const FixedPoint &resource_quantity = resource_pair.second;

    auto it = available_resources_.find(resource_name);
    RAY_CHECK(it != available_resources_.end());
    acquired_resources[resource_name] = it->second.Acquire(resource_quantity);
    if (it->second.TotalQuantityIsZero()) {
      available_resources_.erase(it);
    }
  }
  return ResourceIdSet(acquired_resources);
}

void ResourceIdSet::Release(const ResourceIdSet &resource_id_set) {
  for (auto const &resource_pair : resource_id_set.AvailableResources()) {
    auto const &resource_name = resource_pair.first;
    auto const &resource_ids = resource_pair.second;
    RAY_CHECK(!resource_ids.TotalQuantityIsZero());

    auto it = available_resources_.find(resource_name);
    if (it == available_resources_.end()) {
      available_resources_[resource_name] = resource_ids;
    } else {
      it->second.Release(resource_ids);
    }
  }
}

void ResourceIdSet::ReleaseConstrained(const ResourceIdSet &resource_id_set,
                                       const ResourceSet &resources_total) {
  for (auto const &resource_pair : resource_id_set.AvailableResources()) {
    auto const &resource_name = resource_pair.first;
    // Release only if the resource exists in resources_total
    if (resources_total.GetResource(resource_name) != 0) {
      auto const &resource_ids = resource_pair.second;
      RAY_CHECK(!resource_ids.TotalQuantityIsZero());

      auto it = available_resources_.find(resource_name);
      if (it == available_resources_.end()) {
        available_resources_[resource_name] = resource_ids;
      } else {
        it->second.Release(resource_ids);
      }
    }
  }
}

void ResourceIdSet::Clear() { available_resources_.clear(); }

ResourceIdSet ResourceIdSet::Plus(const ResourceIdSet &resource_id_set) const {
  ResourceIdSet resource_id_set_to_return(available_resources_);
  resource_id_set_to_return.Release(resource_id_set);
  return resource_id_set_to_return;
}

void ResourceIdSet::AddOrUpdateResource(const std::string &resource_name,
                                        int64_t capacity) {
  auto it = available_resources_.find(resource_name);
  if (it != available_resources_.end()) {
    // If resource exists, update capacity
    ResourceIds &resid = (it->second);
    resid.UpdateCapacity(capacity);
  } else {
    // If resource does not exist, create
    available_resources_[resource_name] = ResourceIds(capacity);
  }
}

void ResourceIdSet::DeleteResource(const std::string &resource_name) {
  if (available_resources_.count(resource_name) != 0) {
    available_resources_.erase(resource_name);
  }
}

const std::unordered_map<std::string, ResourceIds> &ResourceIdSet::AvailableResources()
    const {
  return available_resources_;
}

ResourceIdSet ResourceIdSet::GetCpuResources() const {
  std::unordered_map<std::string, ResourceIds> cpu_resources;

  auto it = available_resources_.find(kCPU_ResourceLabel);
  if (it != available_resources_.end()) {
    cpu_resources.insert(*it);
  }
  return ResourceIdSet(cpu_resources);
}

ResourceSet ResourceIdSet::ToResourceSet() const {
  std::unordered_map<std::string, FixedPoint> resource_set;
  for (auto const &resource_pair : available_resources_) {
    resource_set[resource_pair.first] = resource_pair.second.TotalQuantity();
  }
  return ResourceSet(resource_set);
}

std::string ResourceIdSet::ToString() const {
  std::string return_string = "AvailableResources: ";

  auto it = available_resources_.begin();

  // Convert the first element to a string.
  if (it != available_resources_.end()) {
    return_string += (it->first + ": {" + it->second.ToString() + "}");
    it++;
  }

  // Add the remaining elements to the string (along with a comma).
  for (; it != available_resources_.end(); ++it) {
    return_string += (", " + it->first + ": {" + it->second.ToString() + "}");
  }

  return return_string;
}

std::vector<flatbuffers::Offset<protocol::ResourceIdSetInfo>> ResourceIdSet::ToFlatbuf(
    flatbuffers::FlatBufferBuilder &fbb) const {
  std::vector<flatbuffers::Offset<protocol::ResourceIdSetInfo>> return_message;
  for (auto const &resource_pair : available_resources_) {
    std::vector<int64_t> resource_ids;
    std::vector<double> resource_fractions;
    for (auto whole_id : resource_pair.second.WholeIds()) {
      resource_ids.push_back(whole_id);
      resource_fractions.push_back(1);
    }

    for (auto const &fractional_pair : resource_pair.second.FractionalIds()) {
      resource_ids.push_back(fractional_pair.first);
      resource_fractions.push_back(fractional_pair.second.Double());
    }

    auto resource_id_set_message = protocol::CreateResourceIdSetInfo(
        fbb, fbb.CreateString(resource_pair.first), fbb.CreateVector(resource_ids),
        fbb.CreateVector(resource_fractions));

    return_message.push_back(resource_id_set_message);
  }

  return return_message;
}

const std::string ResourceIdSet::Serialize() const {
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(protocol::CreateResourceIdSetInfos(fbb, fbb.CreateVector(ToFlatbuf(fbb))));
  return std::string(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
}

/// SchedulingResources class implementation

SchedulingResources::SchedulingResources()
    : resources_total_(ResourceSet()),
      resources_available_(ResourceSet()),
      resources_load_(ResourceSet()) {}

SchedulingResources::SchedulingResources(const ResourceSet &total)
    : resources_total_(total),
      resources_available_(total),
      resources_load_(ResourceSet()) {}

SchedulingResources::~SchedulingResources() {}

const ResourceSet &SchedulingResources::GetAvailableResources() const {
  return resources_available_;
}

void SchedulingResources::SetAvailableResources(ResourceSet &&newset) {
  resources_available_ = newset;
}

const ResourceSet &SchedulingResources::GetTotalResources() const {
  return resources_total_;
}

void SchedulingResources::SetTotalResources(ResourceSet &&newset) {
  resources_total_ = newset;
}

const ResourceSet &SchedulingResources::GetLoadResources() const {
  return resources_load_;
}

void SchedulingResources::SetLoadResources(ResourceSet &&newset) {
  resources_load_ = newset;
}

// Return specified resources back to SchedulingResources.
void SchedulingResources::Release(const ResourceSet &resources) {
  return resources_available_.AddResourcesCapacityConstrained(resources,
                                                              resources_total_);
}

// Take specified resources from SchedulingResources.
void SchedulingResources::Acquire(const ResourceSet &resources) {
  resources_available_.SubtractResourcesStrict(resources);
}

// The reason we need this method is sometimes we may want add some converted
// resource which is not exist in total resource to the available resource.
// (e.g., placement group)
void SchedulingResources::AddResource(const ResourceSet &resources) {
  resources_total_.AddResources(resources);
  resources_available_.AddResources(resources);
}

void SchedulingResources::UpdateResourceCapacity(const std::string &resource_name,
                                                 int64_t capacity) {
  const FixedPoint new_capacity = FixedPoint(capacity);
  const FixedPoint &current_capacity = resources_total_.GetResource(resource_name);
  if (current_capacity > 0) {
    // If the resource exists, add to total and available resources
    const FixedPoint capacity_difference = new_capacity - current_capacity;
    const FixedPoint &current_available_capacity =
        resources_available_.GetResource(resource_name);
    FixedPoint new_available_capacity = current_available_capacity + capacity_difference;
    if (new_available_capacity < 0) {
      new_available_capacity = 0;
    }
    resources_total_.AddOrUpdateResource(resource_name, new_capacity);
    resources_available_.AddOrUpdateResource(resource_name, new_available_capacity);
  } else {
    // Resource does not exist, just add it to total and available. Do not add to load.
    resources_total_.AddOrUpdateResource(resource_name, new_capacity);
    resources_available_.AddOrUpdateResource(resource_name, new_capacity);
  }
}

void SchedulingResources::DeleteResource(const std::string &resource_name) {
  resources_total_.DeleteResource(resource_name);
  resources_available_.DeleteResource(resource_name);
  resources_load_.DeleteResource(resource_name);
}

const ResourceSet &SchedulingResources::GetNormalTaskResources() const {
  return resources_normal_tasks_;
}

void SchedulingResources::SetNormalTaskResources(const ResourceSet &newset) {
  resources_normal_tasks_ = newset;
}

std::string SchedulingResources::DebugString() const {
  std::stringstream result;

  auto resources_available = resources_available_;
  resources_available.SubtractResources(resources_normal_tasks_);

  result << "\n- total: " << resources_total_.ToString();
  result << "\n- avail: " << resources_available.ToString();
  result << "\n- normal task usage: " << resources_normal_tasks_.ToString();
  return result.str();
};

}  // namespace ray
