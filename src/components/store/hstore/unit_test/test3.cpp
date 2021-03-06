#include "store_map.h"

#include <gtest/gtest.h>
#include <common/utils.h>
#include <api/components.h>
/* note: we do not include component source, only the API definition */
#include <api/kvstore_itf.h>

#if defined HAS_PROFILER
#include <gperftools/profiler.h> /* Alas, no __has_include until C++17 */
#else
int ProfilerStart(const char *) {}
void ProfilerStop() {}
#endif

#include <chrono>
#include <cstdlib>
#include <functional> /* function */
#include <iostream>
#include <string>
#include <random>
#include <sstream>
#include <tuple>
#include <vector>

class timer
{
public:
	using clock_t = std::chrono::steady_clock;
	using duration_t = typename clock_t::duration;
private:
	std::function<void(duration_t) noexcept> _f;
	clock_t::time_point _start;
public:
	timer(std::function<void(duration_t) noexcept> f_)
		: _f(f_)
		, _start(clock_t::now())
	{}
	~timer()
	{
		_f(clock_t::now() - _start);
	}
};

/*
 * Performancte test:
 *
 * export PMEM_IS_PMEM_FORCE=1
 * LD_PRELOAD=/usr/lib/libprofiler.so CPUPROFILE=cpu.profile ./src/components/hstore/unit_test/hstore-test3
 *
 * memory requirement:
 *   memory (MB)  entries   key_sz  value_sz
 *    MB(128)       67484     8        16
 *    MB(512)      459112     8        16
 *    MB(2048UL)  2028888     8        16
 *    MB(2048UL)   918925    16        32
 *    MB(4096UL) >2000000    16        32
 *
 * (this table obsoleted, or at least made harder to generate, but use of multiple key and data sizes)
 */

using namespace Component;

namespace {

// The fixture for testing class Foo.
class KVStore_test
  : public ::testing::Test
{
  static constexpr std::size_t estimated_object_count_large = 64000000;
  /* More testing of table splits, at a performance cost */
  static constexpr std::size_t estimated_object_count_small = 1;

  static constexpr std::size_t many_count_target_large = 2000000;
  /* Shorter test: use when PMEM_IS_PMEM_FORCE=0 */
  static constexpr std::size_t many_count_target_small = 400;

 protected:

  // If the constructor and destructor are not enough for setting up
  // and cleaning up each test, you can define the following methods:

  virtual void SetUp() {
    // Code here will be called immediately after the constructor (right
    // before each test).
  }

  virtual void TearDown() {
    // Code here will be called immediately after each test (right
    // before the destructor).
  }

  // Objects declared here can be used by all tests in the test case

  /* persistent memory if enabled at all, is simulated and not real */
  static bool pmem_simulated;
  static Component::IKVStore * _kvstore;
  static Component::IKVStore::pool_t pool;

  static constexpr unsigned many_key_length_short = 16;
  static constexpr unsigned many_key_length_long = 32;
  static constexpr unsigned many_value_length_short = 16;
  static constexpr unsigned many_value_length_long = 128;
  using kv_t = std::tuple<std::string, std::string>;
  using kvv_t = std::vector<std::tuple<std::string, std::string>>;
  /* data (puts in particular) are tested in 3 combinations:
   * short short (key and data both fit in the hash table space)
   * short long (key fits in hash table space, data needs a memory allocation)
   * long long (key and data both need memory allocations)
   *
   * The current limit for fit in hash table is 23 bytes, for both key and data.
   */
  static kvv_t kvv_short_short;
  static kvv_t kvv_short_long;
  static kvv_t kvv_long_long;

  static std::size_t multi_count_actual;
  static std::size_t estimated_object_count;
  static std::size_t many_count_target;
  static void populate_many(kvv_t &kvv, char tag, std::size_t key_length, std::size_t value_length);
  static long unsigned put_many(const kvv_t &kvv, const std::string &descr);
  static void get_many(const kvv_t &kvv, const std::string &descr);

  std::string pool_dir() const
  {
    return "/mnt/pmem0/pool/0/";
  }

  std::string pool_name() const
  {
    return "test-" + store_map::impl->name + store_map::numa_zone() + ".pool";
  }
};

constexpr std::size_t KVStore_test::estimated_object_count_large;
constexpr std::size_t KVStore_test::estimated_object_count_small;
constexpr std::size_t KVStore_test::many_count_target_large;
constexpr std::size_t KVStore_test::many_count_target_small;

bool KVStore_test::pmem_simulated = getenv("PMEM_IS_PMEM_FORCE");
Component::IKVStore *KVStore_test::_kvstore;
Component::IKVStore::pool_t KVStore_test::pool;

constexpr unsigned KVStore_test::many_key_length_short;
constexpr unsigned KVStore_test::many_key_length_long;
constexpr unsigned KVStore_test::many_value_length_short;
constexpr unsigned KVStore_test::many_value_length_long;
KVStore_test::kvv_t KVStore_test::kvv_short_short;
KVStore_test::kvv_t KVStore_test::kvv_short_long;
KVStore_test::kvv_t KVStore_test::kvv_long_long;

std::size_t KVStore_test::multi_count_actual = 0;
std::size_t KVStore_test::estimated_object_count = KVStore_test::pmem_simulated ? estimated_object_count_small : estimated_object_count_large;
std::size_t KVStore_test::many_count_target = KVStore_test::pmem_simulated ? many_count_target_small : many_count_target_large;

TEST_F(KVStore_test, Instantiate)
{
  /* create object instance through factory */
  auto link_library = "libcomanche-" + store_map::impl->name + ".so";
  Component::IBase * comp = Component::load_component(link_library,
                                                      store_map::impl->factory_id);

  ASSERT_TRUE(comp);
  auto fact = static_cast<IKVStore_factory *>(comp->query_interface(IKVStore_factory::iid()));

  _kvstore = fact->create("owner", "name", store_map::location);

  fact->release_ref();
}

TEST_F(KVStore_test, RemoveOldPool)
{
  if ( _kvstore )
  {
    try
    {
      _kvstore->delete_pool(pool_dir(), pool_name());
    }
    catch ( Exception & )
    {
    }
  }
}

TEST_F(KVStore_test, CreatePool)
{
  ASSERT_TRUE(_kvstore);
  pool = _kvstore->create_pool(pool_dir(), pool_name(), MB(16381UL), 0, estimated_object_count);
  ASSERT_LT(0, int64_t(pool));
}

void KVStore_test::populate_many(kvv_t &kvv, const char tag, std::size_t key_length, std::size_t value_length)
{
  std::mt19937_64 r0{};
  for ( auto i = 0; i != many_count_target; ++i )
  {
    auto ukey = r0();
    std::ostringstream s;
    s << tag << std::hex << ukey;
    auto key = s.str();
    key.resize(key_length, '.');
    auto value = std::to_string(i);
    value.resize(value_length, '.');
    kvv.emplace_back(key, value);
  }
}

TEST_F(KVStore_test, PopulateManySmallSmall)
{
  populate_many(kvv_short_short, 'A', many_key_length_short, many_value_length_short);
}

TEST_F(KVStore_test, PopulateManySmallLarge)
{
  populate_many(kvv_short_long, 'B', many_key_length_short, many_value_length_long);
}

TEST_F(KVStore_test, PopulateManyLargeLarge)
{
  populate_many(kvv_long_long, 'C', many_key_length_long, many_value_length_long);
}

long unsigned KVStore_test::put_many(const kvv_t &kvv, const std::string &descr)
{
  long unsigned count = 0;
  ProfilerStart(("test3-put-" + descr + "-cpu-" + store_map::impl->name + ".profile").c_str());
  {
    timer t(
      [&count, &descr] (timer::duration_t d) {
        double seconds = std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1e6;
        std::cerr << descr << " " << count / seconds << " per second\n";
      }
    );
    for ( auto &kv : kvv )
    {
      const auto &key = std::get<0>(kv);
      const auto &value = std::get<1>(kv);
      auto r = _kvstore->put(pool, key, value.data(), value.length());
      if ( r == S_OK )
      {
          ++count;
      }
      else
      {
         std::cerr << __func__ << " FAIL " << key << "\n";
      }
    }
  }
  ProfilerStop();
  return count;
}

TEST_F(KVStore_test, PutManyShortShort)
{
  ASSERT_NE(_kvstore, nullptr);
  ASSERT_LT(0, int64_t(pool));

  auto count_actual = put_many(kvv_short_short, "short_short");

  EXPECT_LE(count_actual, many_count_target);
  EXPECT_LE(many_count_target * 0.99, double(count_actual));

  multi_count_actual += count_actual;
}

TEST_F(KVStore_test, PutManyShortLong)
{
  ASSERT_NE(_kvstore, nullptr);
  ASSERT_LT(0, int64_t(pool));

  auto count_actual = put_many(kvv_short_long, "short_long");

  EXPECT_LE(count_actual, many_count_target);
  EXPECT_LE(many_count_target * 0.99, double(count_actual));

  multi_count_actual += count_actual;
}

TEST_F(KVStore_test, PutManyLongLong)
{
  ASSERT_NE(_kvstore, nullptr);
  ASSERT_LT(0, int64_t(pool));

  auto count_actual = put_many(kvv_long_long, "long_long");

  EXPECT_LE(count_actual, many_count_target);
  EXPECT_LE(many_count_target * 0.99, double(count_actual));

  multi_count_actual += count_actual;
}

TEST_F(KVStore_test, Size1)
{
  ASSERT_NE(_kvstore, nullptr);
  ASSERT_LT(0, int64_t(pool));

  auto count = _kvstore->count(pool);
  /* count should reflect PutMany */
  EXPECT_EQ(count, multi_count_actual);
}

void KVStore_test::get_many(const kvv_t &kvv, const std::string &descr)
{
  ProfilerStart(("test3-get-" + descr + "-cpu-" + store_map::impl->name + ".profile").c_str());
  /* get is quick; run 10 for better profiling */
  {
    unsigned amplification = 10;
	auto ct = amplification * kvv.size();
    timer t(
		[&descr,ct] (timer::duration_t d) {
			double seconds = std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1e6;
			std::cerr << descr << " " << ct << " in " << seconds << " => " << ct / seconds << " per second\n";
		}
	);
    for ( auto i = 0; i != amplification; ++i )
    {
      for ( auto &kv : kvv )
      {
        const auto &key = std::get<0>(kv);
        void * value = nullptr;
        size_t value_len = 0;
        auto r = _kvstore->get(pool, key, value, value_len);
        EXPECT_EQ(r, S_OK);
        EXPECT_EQ(value_len, std::get<1>(kv).size());
        _kvstore->free_memory(value);
      }
    }
  }
  ProfilerStop();
}

TEST_F(KVStore_test, GetManyShortShort)
{
  ASSERT_NE(_kvstore, nullptr);
  ASSERT_LT(0, int64_t(pool));

  get_many(kvv_short_short, "short_short");
}

TEST_F(KVStore_test, GetManyShortLong)
{
  ASSERT_NE(_kvstore, nullptr);
  ASSERT_LT(0, int64_t(pool));

  get_many(kvv_short_long, "short_long");
}

TEST_F(KVStore_test, GetManyLongLong)
{
  ASSERT_NE(_kvstore, nullptr);
  ASSERT_LT(0, int64_t(pool));

  get_many(kvv_long_long, "long_long");
}

TEST_F(KVStore_test, DeletePool)
{
  if ( _kvstore && 0 < int64_t(pool) )
  {
    _kvstore->delete_pool(pool);
  }
}

} // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  auto r = RUN_ALL_TESTS();

  return r;
}
