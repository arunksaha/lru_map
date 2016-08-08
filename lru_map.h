/* 
 * Copyright: Arun Saha <arunksaha@gmail.com>
 *
 * This file provides a generic LRU (Least Recently Used) Map with
 * cost free client-specifiable policies.
 *
 * The LRU class template, LruMap, maintains a mapping from (generic) KeyType
 * to (generic) ValueType. It maintains only the most recently used
 * 'capacity' elements where capacity is specified as a constructor argument.
 * When the map is full, a request to insert a new element discards the
 * least recently used element.
 *
 * The basic design consists of a map and a list. The map maintains the
 * key to value mappings. The list maintains the order or usage, the most
 * recent one is at front, and the least recent one is at back. There are
 * three basic APIs: (1) Insert, (2) Find, and (3) Erase. When a request
 * to Find() is successful, the element is considered used and moved to
 * the front (most recent or hot) of the list. Similarly for Insert() when
 * the key was already existing.
 *
 * Beyond the basics, there are couple of possibilities that appear into
 * the design space, although not all of them may be applicable to a single
 * client. Examples include:
 *
 *  1 Is the class thread-safe?
 *
 *    Thread safety has certain cost. Some clients may be willing to pay
 *    the cost while others, which are already operating in a thread safe
 *    context, may not.
 *
 *  2 Can we maintain the timestamps when an element was inserted and
 *    last accessed?
 *
 *  3 Can we maintain a counter to count how many times an element is accessed?
 *
 *  4 Can we log when some or all of the interesting events among
 *    Insert|Overflow|Find|Erase occur?
 *
 * All these are policies -- independent policies -- that a client may want.
 *
 * One way to address this is to roll out an Options struct with one or more
 * variables for each of the policies. A client constructs an object of that
 * struct and populates the member variables suitable to her needs. The
 * object is then passed as a constructor argument to the main class
 * (here, LruMap) where it is saved as a (const) member variable.
 * Then on, the class chooses its behavior in accordance to passed options.
 *
 * Another way to address this is to introduce gflags corresponding to each
 * option.
 *
 * Athough these ways work quite well, they are in friction with
 * C++'s "Zero Overhead Principle", a principle outlined in
 * "The C++ Programming Language, 4th Edition". The principle is also known
 * as "You don't pay for what you don't need".
 *
 * Why so?
 *
 *  - Because member variables may need to be maintained even when they
 *    are not needed by all clients.
 *
 *  - Because run time checks may need to be performed even when they
 *    are not needed by all clients.
 *
 * This design of this class explores an alternate route. It allows the
 * clients to specify the policies with compile time template parameters.
 *
 * Thus, the LruMap class template accepts template parameters for each
 * of the independent policies mentioned above:
 *  - Locking (thread-safety) (Note 0)
 *  - Timestamps (element access time, modify time)
 *  - HitCounter (element access counter)
 *  - Logging (logging API calls)
 *
 * The default behavior of LruMap is to choose the default behavior for
 * each of the policies. The respective policy classes offering the
 * default behavior is provided at the end of this file. In general, the
 * default policy classes choose the empty/none/least-cost alternative.
 * While instantiating LruMap, a client may throw in a policy class of
 * her own (Note 1). In addition to the default policies, this file
 * also provides couple of non-default policies. As an example, the
 * unit test class instantiates different combinations of those.
 *
 * As a result of choosing this design route, the class makes heavy use
 * of templates. There is a simple pattern that is followed.
 *
 * If a class, say Host, wants to capture some behavior as a configurable
 * policy, then it constructs a template class to capture the policy,
 * say Policy, and inherits from it.
 *
 *   template<template <class> class Policy>
 *   class Host;
 *
 * In most cases, a suitable default policy is supplied, e.g.
 *
 *   template <class> class LockingPolicy = LockNone,
 *
 * A policy class template offers a specific policy. The offering may be
 * through the constructor (as in the case of LockingPolicy classes) or,
 * more usually, through a public static method.
 * For a specific policy, competing policy class templates offer different
 * implementations. One requirement is that the signatures of those methods
 * (or, constructors) must match. As an example, the following three classes
 * offer three different behaviors of LockingPolicy:
 *
 *  A) LockNone: No locking. This may be suitable for single threaded apps
 *               or callers that have higher level locks.
 *
 *  B) LockExclusiveStd: Exclusive locking using std::mutex.
 *
 * Note that offerings of different policy classes can be semantically
 * different (e.g. A vs B), or semantically same but with
 * different mechanism (e.g. B vs. something like 'LockExclusivePthread').
 *
 * Finally, in a member function of Host, where the configurable policy
 * behavior is desired, a call is made to the static method of the policy
 * class.
 *
 * Note 0: Ideally, there should be one class for locking policy, but at
 * present, LruMap has two.
 *
 * Note 1: Every policy class has an interface requirement. Fortunately,
 * if a policy class does not adhere to the requirement, it will generate
 * a compilation error.
 *
 */

#ifndef _LRU_MAP_H_
#define _LRU_MAP_H_

#include <chrono>
#include <limits>
#include <list>
#include <mutex>
#include <sstream>
#include <unordered_map>

// Necessary package: glog
#include "glog/logging.h"

// Forward declaration for the default lock storage policy, see details below.
template <class T> struct LockStorageNone;

// Forward declaration for the default locking policy, see details below.
template <class T> struct LockNone;

// Forward declaration for the default timestamp policy, see details below.
template <class T> struct TimestampNone;

// Forward declaration for the default hit counting policy, see details below.
template <class T> struct HitCountDisabled;

// Forward declaration for the default logging policy, see details below.
template <class T> struct LogEventNone;

// A simple structure to count the number of times different APIs are called.
struct LruMapStats {
  int64_t num_insert{0};    // # of calls to insert.
  int64_t num_overflow{0};  // # of times insertion pushed out the LRU element.
  int64_t num_find{0};      // # of calls to find, both successful and not.
  int64_t num_find_ok{0};   // # of calls to find, only successful.
  int64_t num_erase{0};     // # of calls to erase, both successful and not.
  int64_t num_clear{0};     // # of calls to clear.
  std::string ToString() const;
};

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy = LockStorageNone,
          template <class> class LockingPolicy = LockNone,
          template <class> class TimestampingPolicy = TimestampNone,
          template <class> class HitCountingPolicy = HitCountDisabled,
          template <class> class LoggingPolicy = LogEventNone>
class LruMap : public LockingStoragePolicy<void> {
 public:
  // Construct an object with specified 'capacity'.
  explicit LruMap(const int64_t capacity);

  ~LruMap() = default;

  // Insert or update an entry with key 'key' and value 'value'.
  //
  // If an entry with 'key' already exists, then it is refreshed to be the most
  // recent entry and the value of the entry would be the new 'value' supplied.
  //
  // If the number of entries had already reached the capacity, then the
  // oldest entry is thrown away.
  void Insert(const KeyType& key, const ValueType& value);

  // Find the entry, if exists, for the key 'key'. The returned pointer may
  // become stale through other operations, the client is required to protect
  // against that, for example by copying the object elsewhere.
  const ValueType *Find(const KeyType& key);

  // Return true iff an entry with 'key' exists, false otherwise.
  bool Exists(const KeyType& key) const;

  // Erase entry with key 'key', if exists.
  void Erase(const KeyType& key);

  // Clear all entries in the map and release all memory.
  void Clear();

  // Return the capacity, i.e. the maximum possible number of entries.
  int64_t Capacity() const;

  // Return the current number of entries.
  int64_t Size() const;

  // Audit all the entries and return true iff LRU property is satisfied,
  // false otherwise. This is effective only if timestamps are maintained,
  // for example by choosing the policy TimestampAll.
  bool Valid() const;

  // Return string representation of this object.
  std::string ToString() const;

  // Return a copy of the statistics.
  LruMapStats lru_map_stats() const;

 private:
  // Sometimes a policy class is templated, but the template is not useful.
  typedef void Dummy;

  typedef LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
    TimestampingPolicy, HitCountingPolicy, LoggingPolicy> ThisType;

  friend struct LockingPolicy<ThisType>;

  template <typename KeyT, typename ValueT>
  struct KeyValueT : public TimestampingPolicy<Dummy>,
                            HitCountingPolicy<Dummy> {
    KeyT key;
    ValueT value;

    KeyValueT(KeyT k, ValueT v) : key{k}, value{v} {}

    std::string ToString() const {
      std::ostringstream oss;
      oss << key << "; " << value;
      return oss.str() +
             TimestampingPolicy<Dummy>::ToString() +
             HitCountingPolicy<Dummy>::ToString() + "\n";
    }
  };

  typedef KeyValueT<KeyType, ValueType> KeyValueEntry;
  typedef std::list<KeyValueEntry> ItemList;
  typedef typename ItemList::iterator ItemListIter;
  typedef std::unordered_map<KeyType, ItemListIter> ItemMap;
  typedef typename ItemMap::iterator ItemMapIter;

 private:
  // Implementation of Size() without applying LockingPolicy.
  int64_t SizePrivate() const;

 private:
  // The capacity, i.e. maximum number of elements at a time.
  const int64_t capacity_{0};

  // Cumulative lifetime stats, persist on Clear().
  LruMapStats lru_stats_;

  // Linked list of elements, the most recent one is at front.
  ItemList lru_list_;

  // Map element keys to element values.
  ItemMap lru_key_map_;
};

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::
  LruMap(const int64_t capacity) : capacity_{capacity} {
  CHECK_GE(capacity, 1);

  LOG(INFO) << "LruMap size of types: KeyType = " << sizeof(KeyType)
            << ", ValueType = " << sizeof(ValueType)
            << ", KeyValueEntry = " << sizeof(KeyValueEntry);
  LOG(INFO) << "LruMap size of members: capacity_ = " << sizeof capacity_
            << ", lru_stats_ = " << sizeof lru_stats_
            << ", lru_list_ = " << sizeof lru_list_
            << ", lru_key_map_ = " << sizeof lru_key_map_
            << ", total = " << sizeof *this;
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
void
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::Insert(
  const KeyType& key, const ValueType& value) {

  LockingPolicy<ThisType> lock{this};

  ItemMapIter map_it = lru_key_map_.find(key);
  if (map_it != lru_key_map_.end()) {
    // If the key exists, then it is moved to the front of the list so that
    // it is considered to be the most recent.
    lru_list_.splice(lru_list_.begin(), lru_list_, map_it->second);

    // Update with the new value.
    lru_list_.begin()->value = value;
  } else {
    // If the key does not exist, then a new entry is constructed and inserted
    // to the front of the list.
    const KeyValueEntry kv_entry{key, value};
    lru_list_.push_front(kv_entry);

    // Also, a new entry is inserted into the map such that the key points to
    // the corresponding (now, first) element in the list.
    DCHECK(lru_key_map_.find(key) == lru_key_map_.end());
    const std::pair<ItemMapIter, bool> result =
      lru_key_map_.insert({key, lru_list_.begin()});
    CHECK(result.second);
  }
  DCHECK(lru_list_.begin()->key == key);

  KeyValueEntry *recent_kv_entry = &*lru_list_.begin();
  LoggingPolicy<KeyValueEntry>::LogInsert(*recent_kv_entry);
  TimestampingPolicy<KeyValueEntry>::UpdateModifyTimestamp(recent_kv_entry);

  // If size exceeds capacity, then throw away the least recent entry.
  if (SizePrivate() > capacity_) {
    ItemListIter oldest = lru_list_.end();
    --oldest;
    KeyValueEntry *oldest_kv_entry = &*oldest;
    lru_stats_.num_overflow += 1;
    LoggingPolicy<KeyValueEntry>::LogOverflow(*oldest_kv_entry);

    lru_key_map_.erase(oldest->key);
    lru_list_.pop_back();
  }

  lru_stats_.num_insert += 1;
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
const ValueType *
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::
  Find(const KeyType& key) {

  LockingPolicy<ThisType> lock{this};

  lru_stats_.num_find += 1;

  ItemMapIter map_it = lru_key_map_.find(key);
  if (map_it == lru_key_map_.end()) {
    return nullptr;
  }

  lru_list_.splice(lru_list_.begin(), lru_list_, map_it->second);
  DCHECK(lru_list_.begin()->key == key);

  lru_stats_.num_find_ok += 1;
  KeyValueEntry *found_kv_entry = &*lru_list_.begin();
  HitCountingPolicy<KeyValueEntry>::IncrementHitCount(found_kv_entry);
  LoggingPolicy<KeyValueEntry>::LogFind(*found_kv_entry);
  TimestampingPolicy<KeyValueEntry>::UpdateAccessTimestamp(found_kv_entry);

  return &lru_list_.begin()->value;
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
inline bool
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::Exists(
  const KeyType& key) const {
  LockingPolicy<ThisType> lock{this};
  return lru_key_map_.find(key) != lru_key_map_.end();
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
void
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::
  Erase(const KeyType& key) {

  LockingPolicy<ThisType> lock{this};

  lru_stats_.num_erase += 1;

  ItemMapIter map_it = lru_key_map_.find(key);
  if (map_it == lru_key_map_.end()) {
    return;
  }

  LoggingPolicy<KeyValueEntry>::LogErase(*map_it->second);

  lru_list_.erase(map_it->second);
  lru_key_map_.erase(map_it);
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
void
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::Clear() {
  LockingPolicy<ThisType> lock{this};
  lru_list_.clear();
  lru_key_map_.clear();
  lru_key_map_.reserve(0);
  lru_stats_.num_clear += 1;
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
inline int64_t
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::
  Capacity() const {
  return capacity_;
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
inline int64_t
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::Size() const {
  LockingPolicy<ThisType> lock{this};
  return SizePrivate();
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
inline int64_t
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::SizePrivate() const {
  DCHECK_EQ(lru_list_.size(), lru_key_map_.size());
  return lru_list_.size();
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
bool
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::Valid() const {
  LockingPolicy<ThisType> lock{this};
  return TimestampingPolicy<KeyValueEntry>::Valid(lru_list_);
}

// ----------------------------------------------------------------------------
template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
std::string
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::
  ToString() const {

  LockingPolicy<ThisType> lock{this};

  std::string result;
  // Heuristic reserve, the excess is trimmed before returning.
  result.reserve(lru_list_.size() * 32);
  result += "key; value| atime; mtime\n";
  for (const KeyValueEntry& kv_entry : lru_list_) {
    result += kv_entry.ToString();
  }
  result += "\n";
  result.shrink_to_fit();
  return result;
}

// ----------------------------------------------------------------------------

template <typename KeyType, typename ValueType,
          template <class> class LockingStoragePolicy,
          template <class> class LockingPolicy,
          template <class> class TimestampingPolicy,
          template <class> class HitCountingPolicy,
          template <class> class LoggingPolicy>
inline LruMapStats
LruMap<KeyType, ValueType, LockingStoragePolicy, LockingPolicy,
  TimestampingPolicy, HitCountingPolicy, LoggingPolicy>::
  lru_map_stats() const {
  return lru_stats_;
}

// ----------------------------------------------------------------------------

std::string LruMapStats::ToString() const {
  std::ostringstream oss;
  oss << "num_insert = " << num_insert;
  oss << ", num_overflow = " << num_overflow;
  oss << ", num_find = " << num_find;
  oss << ", num_find_ok = " << num_find_ok;
  oss << ", num_erase = " << num_erase;
  oss << ", num_clear = " << num_clear;
  return oss.str();
}

// ----------------------------------------------------------------------------
//                            LockingStoragePolicy
// ----------------------------------------------------------------------------

template <class T>
struct LockStorageNone {
};

// ----------------------------------------------------------------------------

template <class T>
struct LockStorageStdMutex {
 protected:
  mutable std::mutex mutex;
};

// ----------------------------------------------------------------------------
//                            LockingPolicy
// ----------------------------------------------------------------------------

template <class T>
struct LockNone {
  explicit LockNone(const T *) {}
};

// ----------------------------------------------------------------------------

template <class T>
struct LockExclusiveStd {
  explicit LockExclusiveStd(const T *object) :
    scoped_mutex_locker{object->mutex} {
  };

  ~LockExclusiveStd() = default;

 private:
  std::lock_guard<std::mutex> scoped_mutex_locker;
};

// ----------------------------------------------------------------------------
//                            TimestampingPolicy
// ----------------------------------------------------------------------------

template <class T>
struct TimestampNone {
  static void UpdateAccessTimestamp(T *) {}
  static void UpdateModifyTimestamp(T *) {}

  // If timestamps are not maintained then there is no way to check validity,
  // so, as a benefit of doubt, the structure is considered valid.
  static bool Valid(const std::list<T>& lru_list) {
    return true;
  }

  std::string ToString() const { return std::string{}; };
};

// ----------------------------------------------------------------------------

static int64_t MicrosecondsSinceEpoch() {
  return std::chrono::system_clock::now().time_since_epoch() / 
         std::chrono::microseconds(1);
}

// ----------------------------------------------------------------------------

template <class T>
struct TimestampAll {
  static void UpdateAccessTimestamp(T *kv_entry) {
    kv_entry->access_time_usecs = MicrosecondsSinceEpoch();
  }
  static void UpdateModifyTimestamp(T *kv_entry) {
    kv_entry->modify_time_usecs = MicrosecondsSinceEpoch();
  }

  // Return 'true' iff the entries in the list are chronologically ordered from
  // newer to older.
  static bool Valid(const std::list<T>& lru_list) {
    int64_t prev_usecs = std::numeric_limits<int64_t>::max();
    for (const T& kv_entry : lru_list) {
      // Find the newer (i.e. larger) one between access time and modify time.
      const int64_t current_recent_usecs =
        std::max<int64_t>(kv_entry.access_time_usecs,
                        kv_entry.modify_time_usecs);

      // Since we are iterating from most recent to least recent,
      // the recent-most timestamp of the current entry must be
      // older or same (i.e. less or equal) than the previous entry.
      // Otherwise, its a violation of the LRU invariant and hence invalid.
      if (current_recent_usecs > prev_usecs) {
        return false;
      }

      prev_usecs = current_recent_usecs;
    }
    return true;
  }

  std::string ToString() const {
    std::ostringstream oss;
    oss << "| atime = " << access_time_usecs
        << "; mtime = " << modify_time_usecs;
    return oss.str();
  }

  // Timestamp of last access through Find().
  int64_t access_time_usecs{0};

  // Timestamp of last mutation through Insert(), either fresh or overwrite.
  int64_t modify_time_usecs{0};
};

// ----------------------------------------------------------------------------
//                            HitCountingPolicy
// ----------------------------------------------------------------------------

template <class T>
struct HitCountDisabled {
  static void IncrementHitCount(T *) {}
  std::string ToString() const { return std::string{}; };
};

// ----------------------------------------------------------------------------

template <class T>
struct HitCountEnabled {
  static void IncrementHitCount(T *kv_entry) {
    kv_entry->hit_count += 1;
  }

  std::string ToString() const {
    std::ostringstream oss;
    oss << "| hit_count = " << hit_count;
    return oss.str();
  }

  int64_t hit_count{0};
};

// ----------------------------------------------------------------------------
//                            LoggingPolicy
// ----------------------------------------------------------------------------

template <class T>
struct LogEventNone {
  static void LogInsert(const T&) {}
  static void LogOverflow(const T&) {}
  static void LogFind(const T&) {}
  static void LogErase(const T&) {}
};

// ----------------------------------------------------------------------------

template <class T>
struct LogEventOverflow {
  static void LogInsert(const T&) {}
  static void LogOverflow(const T& kv_entry) {
    Log("Overflow", kv_entry);
  }
  static void LogFind(const T&) {}
  static void LogErase(const T&) {}
 private:
  static void Log(const std::string& event, const T& kv_entry) {
    LOG(INFO) << event << ": " << kv_entry.ToString();
  }
};

// ----------------------------------------------------------------------------

template <class T>
struct LogEventAll {
  static void LogInsert(const T& kv_entry) {
    Log("Insert", kv_entry);
  }
  static void LogOverflow(const T& kv_entry) {
    Log("Overflow", kv_entry);
  }
  static void LogFind(const T& kv_entry) {
    Log("Find", kv_entry);
  }
  static void LogErase(const T& kv_entry) {
    Log("Erase", kv_entry);
  }
 private:
  static void Log(const std::string& event, const T& kv_entry) {
    LOG(INFO) << event << ": " << kv_entry.ToString();
  }
};

// ----------------------------------------------------------------------------

#endif // _LRU_MAP_H_
