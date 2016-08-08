#include <random>
#include "lru_map.h"

using namespace std;

struct LruKey {
  explicit LruKey(int64_t k) : key(k) {}
  bool operator==(const LruKey& rhs) const {
    return key == rhs.key;
  }
  int64_t key;
};

std::ostream& operator<<(std::ostream& os, const LruKey& lrukey) {
  os << lrukey.key;
  return os;
}

namespace std {
template <>
struct hash<LruKey> {
  size_t operator()(const LruKey& lrukey) const {
    return hash<int64_t>()(lrukey.key);
  }
};
}

struct LruValue {
  explicit LruValue(int32_t v) : value(v) {}
  int32_t value;
};

std::ostream& operator<<(std::ostream& os, const LruValue& lruvalue) {
  os << lruvalue.value;
  return os;
}


// Generate an uniformly distributed random number between 0 and 1000.
static int64_t
RandomUniformNumber() {
  static std::default_random_engine generator;
  static std::uniform_int_distribution<int64_t> distribution{0, 1000};
  return distribution(generator);
}

template <class LruMapType>
class LruMapTest {
 public:
  explicit LruMapTest(int64_t capacity);
  ~LruMapTest() = default;
  void Test();

 private:
  static LruValue KeyToValue(const LruKey& key);

  void MatchExpectations(int begin_index, int end_index, bool expectation);

  void Insert(const LruKey& key, const LruValue& value);

 private:
  const int64_t capacity_{0};
  LruMapType cache_;
};


template <class LruMapType>
LruMapTest<LruMapType>::LruMapTest(const int64_t capacity) :
  capacity_{capacity}, cache_{capacity_} {
}


template <class LruMapType>
void LruMapTest<LruMapType>::Test() {

  CHECK(cache_.Valid());
  CHECK_EQ(cache_.Capacity(), capacity_);
  CHECK_EQ(cache_.Size(), 0);

  // Lookup of elements in range [0, N) is expected to fail.
  MatchExpectations(0, capacity_, false);

  // Insert N [0, N) elements.
  for (int idx = 0; idx != capacity_; ++idx) {
    const LruKey key{idx};
    const LruValue value{KeyToValue(key)};
    Insert(key, value);
    CHECK_EQ(cache_.Size(), idx + 1);
  }
  CHECK_EQ(cache_.Size(), capacity_);

  // Lookup of elements in range [0, N) is expected to succeed.
  MatchExpectations(0, capacity_, true);
  LOG(INFO) << cache_.ToString();

  // Insert N [N, 2N) more elements.
  for (int idx = capacity_; idx != 2*capacity_; ++idx) {
    const LruKey key{idx};
    const LruValue value{KeyToValue(key)};
    Insert(key, value);
    CHECK_EQ(cache_.Size(), capacity_);
  }

  // Now, lookup of elements in range [0, N) is expected to fail.
  MatchExpectations(0, capacity_, false);

  // But, lookup of elements in range [N, 2*N) is expected to succeed.
  MatchExpectations(capacity_, 2*capacity_, true);

  LOG(INFO) << "Find and Erase";
  usleep(RandomUniformNumber());
  LOG(INFO) << "Original: " << cache_.ToString();
  cache_.Valid();
  const LruKey kx{2*capacity_ - 1};
  // First, find and ensure that 'kx' exists.
  const LruValue *const vx = cache_.Find(kx);
  cache_.Valid();
  CHECK(vx);
  // Then, erase 'kx'.
  cache_.Erase(kx);
  LOG(INFO) << "After Erase: " << cache_.ToString();
  cache_.Valid();
  // Finally, find and ensure that 'kx' does not exist anymore.
  const LruValue *const vx2 = cache_.Find(kx);
  cache_.Valid();
  CHECK(!vx2);

  LOG(INFO) << "Overwrite Insert";
  const LruKey kx3{2*capacity_ - 2};
  Insert(kx3, LruValue(2016));

  LOG(INFO) << "Stats: " << cache_.lru_map_stats().ToString();
}


template <class LruMapType>
void LruMapTest<LruMapType>::MatchExpectations(
  const int begin_index,
  const int end_index,
  const bool expectation) {

  CHECK_LE(begin_index, end_index);

  for (int idx = begin_index; idx != end_index; ++idx) {
    const LruKey key(idx);

    const bool found = cache_.Exists(key);
    CHECK_EQ(found, expectation);

    const LruValue *found_value = cache_.Find(key);
    if (expectation) {
      CHECK(found_value);
      CHECK_EQ(found_value->value, KeyToValue(key).value);
    } else {
      CHECK(!found_value);
    }

    CHECK(cache_.Valid());
  }
}


template <class LruMapType>
void LruMapTest<LruMapType>::Insert(
  const LruKey& key,
  const LruValue& value) {

  // Sleep a random amount of time to emulate passage of time.
  usleep(RandomUniformNumber());

  LOG(INFO) << "Inserting key: " << key << ", value: " << value;
  cache_.Insert(key, value);
  LOG(INFO) << cache_.ToString();

  CHECK(cache_.Valid());
}


template <class LruMapType>
LruValue LruMapTest<LruMapType>::KeyToValue(const LruKey& key) {
  return LruValue(5 * key.key);
}


// ----------------------------------------------------------------------------

static const int64_t kLruCapacity = 4;

void Test1() {
  LOG(INFO) << "Testing with default policies";
  typedef LruMap<LruKey, LruValue> MyLruMapType;
  LruMapTest<MyLruMapType> test{kLruCapacity};
  test.Test();
}


void Test2() {
  LOG(INFO) << "Testing with TimestampAll";
  typedef LruMap<LruKey, LruValue, LockStorageNone, LockNone, TimestampAll>
    MyLruMapType;
  LruMapTest<MyLruMapType> test{kLruCapacity};
  test.Test();
}


void Test3() {
  LOG(INFO) << "Testing with TimestampAll + HitCountEnabled";
  typedef LruMap<LruKey, LruValue, LockStorageNone, LockNone, TimestampAll,
    HitCountEnabled> MyLruMapType;
  LruMapTest<MyLruMapType> test{kLruCapacity};
  test.Test();
}


void Test4() {
  LOG(INFO) << "Testing with TimestampAll + HitCountEnabled"
            << " + LogEventOverflow";
  typedef LruMap<LruKey, LruValue, LockStorageNone, LockNone, TimestampAll,
    HitCountEnabled, LogEventOverflow> MyLruMapType;
  LruMapTest<MyLruMapType> test{kLruCapacity};
  test.Test();
}


void Test5() {
  LOG(INFO) << "Testing with LockStorageStdMutex + LockExclusiveStd + "
            << "TimestampAll + HitCountEnabled + LogEventOverflow";
  typedef LruMap<LruKey, LruValue, LockStorageStdMutex, LockExclusiveStd,
    TimestampAll, HitCountEnabled, LogEventAll> MyLruMapType;
  LruMapTest<MyLruMapType> test{kLruCapacity};
  test.Test();
}


int main(int argc, char *argv[]) {
  Test1();
  Test2();
  Test3();
  Test4();
  Test5();

  LOG(INFO) << "All tests passed";
}
