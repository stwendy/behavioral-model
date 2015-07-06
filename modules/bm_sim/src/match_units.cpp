/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

#include "bm_sim/match_units.h"
#include "bm_sim/match_tables.h"

#define HANDLE_VERSION(h) (h >> 32)
#define HANDLE_INTERNAL(h) (h && 0xffffffff)
#define HANDLE_SET(v, i) ((((uint64_t) v) << 32) | i)

template<typename V>
MatchErrorCode
MatchUnitAbstract<V>::get_and_set_handle(internal_handle_t *handle)
{
  if(num_entries >= size) { // table is full
    return MatchErrorCode::TABLE_FULL;
  }
  
  if(handles.get_handle(handle)) return MatchErrorCode::ERROR;
  
  num_entries++;
  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitAbstract<V>::unset_handle(internal_handle_t handle)
{
  if(handles.release_handle(handle)) return MatchErrorCode::INVALID_HANDLE;
  
  num_entries--;
  return MatchErrorCode::SUCCESS;
}

template<typename V>
bool
MatchUnitAbstract<V>::valid_handle_(internal_handle_t handle) const
{
  return handles.valid_handle(handle);
}

template<typename V>
bool
MatchUnitAbstract<V>::valid_handle(entry_handle_t handle) const
{
  return this->valid_handle_(HANDLE_INTERNAL(handle));
}

template<typename V>
typename MatchUnitAbstract<V>::MatchUnitLookup
MatchUnitAbstract<V>::lookup(const Packet &pkt) const
{
  static thread_local ByteContainer key;
  key.clear();
  build_key(*pkt.get_phv(), key);

  return lookup_key(key);
}

template<typename V>
typename MatchUnitExact<V>::MatchUnitLookup
MatchUnitExact<V>::lookup_key(const ByteContainer &key) const
{
  const auto entry_it = entries_map.find(key);
  // std::cout << "looking up: " << key.to_hex() << "\n";
  if(entry_it == entries_map.end()) return MatchUnitLookup::empty_entry();
  return MatchUnitLookup(entry_it->second, &entries[entry_it->second].value);
}

template<typename V>
MatchErrorCode
MatchUnitExact<V>::add_entry(
  const std::vector<MatchKeyParam> &match_key, V value,
  entry_handle_t *handle, int priority
)
{
  (void) priority;

  ByteContainer new_key;
  new_key.reserve(this->nbytes_key);

  // take care of valid first

  for(const MatchKeyParam &param : match_key) {
    if(param.type == MatchKeyParam::Type::VALID)
      new_key.append(param.key);
  }

  for(const MatchKeyParam &param : match_key) {
    switch(param.type) {
    case MatchKeyParam::Type::EXACT:
      new_key.append(param.key);
      break;
    case MatchKeyParam::Type::VALID: // already done
      break;
    default:
      assert(0 && "invalid param type in match_key");
      break;
    }
  }

  assert(new_key.size() == this->nbytes_key);

  internal_handle_t handle_;
  MatchErrorCode status = this->get_and_set_handle(&handle_);
  if(status != MatchErrorCode::SUCCESS) return status;
  
  uint32_t version = entries[handle_].version;
  *handle = HANDLE_SET(version, handle_);

  entries_map[new_key] = handle_; // key is copied, which is not great
  entries[handle_] = Entry(std::move(new_key), std::move(value), version);
  
  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitExact<V>::delete_entry(entry_handle_t handle)
{
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if(!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if(HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.version += 1;
  entries_map.erase(entry.key);

  return this->unset_handle(handle_);
}

template<typename V>
MatchErrorCode
MatchUnitExact<V>::modify_entry(entry_handle_t handle, V value)
{
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if(!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if(HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.value = std::move(value);

  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitExact<V>::get_value(entry_handle_t handle, const V **value)
{
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if(!this->valid_handle(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if(HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  *value = &entry.value;

  return MatchErrorCode::SUCCESS;
}

template<typename V>
void
MatchUnitExact<V>::dump(std::ostream &stream) const
{
  for(internal_handle_t handle_ : this->handles) {
    const Entry &entry = entries[handle_];
    stream << handle_ << ": " << entry.key.to_hex() << " => ";
    entry.value.dump(stream);
    stream << "\n";
  }
}

template<typename V>
typename MatchUnitLPM<V>::MatchUnitLookup
MatchUnitLPM<V>::lookup_key(const ByteContainer &key) const
{
  entry_handle_t handle;
  if(entries_trie.lookup(key, &handle)) {
    return MatchUnitLookup(handle, &entries[handle].value);
  }
  return MatchUnitLookup::empty_entry();
}

template<typename V>
MatchErrorCode
MatchUnitLPM<V>::add_entry(
  const std::vector<MatchKeyParam> &match_key, V value,
  entry_handle_t *handle, int priority
)
{
  (void) priority;

  ByteContainer new_key;
  new_key.reserve(this->nbytes_key);
  int prefix_length = 0;
  const MatchKeyParam *lpm_param = nullptr;

  for(const MatchKeyParam &param : match_key) {
    if(param.type == MatchKeyParam::Type::VALID)
      new_key.append(param.key);
  }

  for(const MatchKeyParam &param : match_key) {
    switch(param.type) {
    case MatchKeyParam::Type::EXACT:
      new_key.append(param.key);
      prefix_length += param.key.size();
      break;
    case MatchKeyParam::Type::LPM:
      assert(!lpm_param && "more than one lpm param in match key");
      lpm_param = &param;
      break;
    case MatchKeyParam::Type::VALID: // already done
      break;
    default:
      assert(0 && "invalid param type in match_key");
      break;
    }
  }

  assert(lpm_param && "no lpm param in match key");
  new_key.append(lpm_param->key);
  prefix_length += lpm_param->prefix_length;

  assert(new_key.size() == this->nbytes_key);

  internal_handle_t handle_;
  MatchErrorCode status = this->get_and_set_handle(&handle_);
  if(status != MatchErrorCode::SUCCESS) return status;

  uint32_t version = entries[handle_].version;
  *handle = HANDLE_SET(version, handle_);
  
  // key is copied, which is not great
  entries_trie.insert_prefix(new_key, prefix_length, handle_);
  entries[handle_] = Entry(std::move(new_key), prefix_length,
			   std::move(value), version);
  
  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitLPM<V>::delete_entry(entry_handle_t handle)
{
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if(!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if(HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.version += 1;
  assert(entries_trie.delete_prefix(entry.key, entry.prefix_length));

  return this->unset_handle(handle_);
}

template<typename V>
MatchErrorCode
MatchUnitLPM<V>::modify_entry(entry_handle_t handle, V value)
{
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if(!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if(HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.value = std::move(value);

  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitLPM<V>::get_value(entry_handle_t handle, const V **value)
{
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if(!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if(HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  *value = &entry.value;

  return MatchErrorCode::SUCCESS;
}

template<typename V>
void
MatchUnitLPM<V>::dump(std::ostream &stream) const
{
  for(internal_handle_t handle_ : this->handles) {
    const Entry &entry = entries[handle_];
    stream << handle_ << ": "
	   << entry.key.to_hex() << "/" << entry.prefix_length << " => ";
    entry.value.dump(stream);
    stream << "\n";
  }
}


template<typename V>
typename MatchUnitTernary<V>::MatchUnitLookup
MatchUnitTernary<V>::lookup_key(const ByteContainer &key) const
{
  int max_priority = 0;
  bool match;

  const Entry *entry;
  const Entry *max_entry = nullptr;
  entry_handle_t max_handle = 0;

  for(auto it = this->handles.begin(); it != this->handles.end(); ++it) {
    entry = &entries[*it];

    if(entry->priority <= max_priority) continue;
    
    match = true;
    for(size_t byte_index = 0; byte_index < this->nbytes_key; byte_index++) {
      if(entry->key[byte_index] != (key[byte_index] & entry->mask[byte_index])) {
	match = false;
	break;
      }
    }

    if(match) {
      max_priority = entry->priority;
      max_entry = entry;
      max_handle = *it;
    }
  }

  if(max_entry) {
    return MatchUnitLookup(max_handle, &max_entry->value);
  }

  return MatchUnitLookup::empty_entry();
}

static std::string create_mask_from_pref_len(int prefix_length, int size) {
  std::string mask(size, '\x00');
  std::fill(mask.begin(), mask.begin() + (prefix_length / 8), '\xff');
  if(prefix_length % 8 != 0) {
    mask[prefix_length / 8] = (char) 0xFF << (8 - (prefix_length % 8));
  }
  return mask;
}

template<typename V>
MatchErrorCode
MatchUnitTernary<V>::add_entry(
  const std::vector<MatchKeyParam> &match_key, V value,
  entry_handle_t *handle, int priority
)
{
  ByteContainer new_key;
  ByteContainer new_mask;
  new_key.reserve(this->nbytes_key);
  new_mask.reserve(this->nbytes_key);

  for(const MatchKeyParam &param : match_key) {
    if(param.type == MatchKeyParam::Type::VALID) {
      new_key.append(param.key);
      new_mask.append("\xff");
    }
  }

  for(const MatchKeyParam &param : match_key) {
    switch(param.type) {
    case MatchKeyParam::Type::EXACT:
      new_key.append(param.key);
      new_mask.append(std::string(param.key.size(), '\xff'));
      break;
    case MatchKeyParam::Type::LPM:
      new_key.append(param.key);
      new_mask.append(create_mask_from_pref_len(param.prefix_length,
						param.key.size()));
      break;
    case MatchKeyParam::Type::TERNARY:
      new_key.append(param.key);
      new_mask.append(param.mask);
      break;
    case MatchKeyParam::Type::VALID: // already done
      break;
    default:
      assert(0 && "invalid param type in match_key");
      break;
    }
  }

  assert(new_key.size() == this->nbytes_key);
  assert(new_mask.size() == this->nbytes_key);

  internal_handle_t handle_;
  MatchErrorCode status = this->get_and_set_handle(&handle_);
  if(status != MatchErrorCode::SUCCESS) return status;

  uint32_t version = entries[handle_].version;
  *handle = HANDLE_SET(version, handle_);
  
  entries[handle_] = Entry(std::move(new_key), std::move(new_mask), priority,
			   std::move(value), version);
  
  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitTernary<V>::delete_entry(entry_handle_t handle)
{
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if(!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if(HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.version += 1;

  return this->unset_handle(handle_);
}

template<typename V>
MatchErrorCode
MatchUnitTernary<V>::modify_entry(entry_handle_t handle, V value)
{
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if(!this->valid_handle(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if(HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.value = std::move(value);

  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitTernary<V>::get_value(entry_handle_t handle, const V **value)
{
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if(!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if(HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  *value = &entry.value;

  return MatchErrorCode::SUCCESS;
}

template<typename V>
void
MatchUnitTernary<V>::dump(std::ostream &stream) const
{
  for(internal_handle_t handle_ : this->handles) {
    const Entry &entry = entries[handle_];
    stream << handle_ << ": "
	   << entry.key.to_hex() << " &&& " << entry.mask.to_hex() << " => ";
    entry.value.dump(stream);
    stream << "\n";
  }
}


// explicit template instantiation

// I did not think I had to explicitly instantiate MatchUnitAbstract, because it
// is a base class for the others, but I get an linker error if I don't
template class MatchUnitAbstract<MatchTableAbstract::ActionEntry>;
template class MatchUnitAbstract<MatchTableIndirect::IndirectIndex>;

template class MatchUnitExact<MatchTableAbstract::ActionEntry>;
template class MatchUnitExact<MatchTableIndirect::IndirectIndex>;

template class MatchUnitLPM<MatchTableAbstract::ActionEntry>;
template class MatchUnitLPM<MatchTableIndirect::IndirectIndex>;

template class MatchUnitTernary<MatchTableAbstract::ActionEntry>;
template class MatchUnitTernary<MatchTableIndirect::IndirectIndex>;
