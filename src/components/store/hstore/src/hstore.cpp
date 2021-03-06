/*
 * (C) Copyright IBM Corporation 2018, 2019. All rights reserved.
 * US Government Users Restricted Rights - Use, duplication or disclosure restricted by GSA ADP Schedule Contract with IBM Corp.
 */

#include "hstore.h"

#define USE_PMEM 0

/*
 * USE_PMEM 1
 *   USE_CC_HEAP 0: allocation from pmemobj pool
 *   USE_CC_HEAP 1: simple allocation using actual addresses from a large region obtained from pmemobj 
 *   USE_CC_HEAP 2: simple allocation using offsets from a large region obtained from pmemobj 
 *   USE_CC_HEAP 3: AVL-based allocation using actual addresses from a large region obtained from pmemobj 
 * USE_PMEM 0
 *   USE_CC_HEAP 1: simple allocation using actual addresses from a large region obtained from dax_map 
 *   USE_CC_HEAP 2: simple allocation using offsets from a large region obtained from dax_map (NOT TESTED)
 *   USE_CC_HEAP 3: AVL-based allocation using actual addresses from a large region obtained from dax_map 
 *
 */
#if USE_PMEM
/* with PMEM, choose the CC_HEAP version: 0, 1, 2, 3 */
#define USE_CC_HEAP 0
#else
/* without PMEM, only heap version 1 or 3 works */
#define USE_CC_HEAP 3
#endif

#if USE_CC_HEAP == 1
#include "allocator_cc.h"
#elif USE_CC_HEAP == 2
#include "allocator_co.h"
#elif USE_CC_HEAP == 3
#include "allocator_rc.h"
#endif
#include "atomic_controller.h"
#include "hop_hash.h"
#include "perishable.h"
#include "persist_fixed_string.h"

#include <stdexcept>
#include <set>

#include <city.h>
#include <common/exceptions.h>
#include <common/logging.h>
#include <common/utils.h>

#if USE_PMEM
#include "hstore_pmem_types.h"
#include "persister_pmem.h"
#else
#include "hstore_nupm_types.h"
#include "persister_nupm.h"
#endif

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring> /* strerror, memcpy */
#include <memory> /* unique_ptr */
#include <new>
#include <map> /* session set */
#include <mutex> /* thread safe use of libpmempool/obj */
#include <stdexcept> /* domain_error */

#define PREFIX "HSTORE : %s: "

#if 0
/* thread-safe hash */
#include <mutex>
using hstore_shared_mutex = std::shared_timed_mutex;
static constexpr auto thread_model = Component::IKVStore::THREAD_MODEL_MULTI_PER_POOL;
#else
/* not a thread-safe hash */
#include "dummy_shared_mutex.h"
using hstore_shared_mutex = dummy::shared_mutex;
static constexpr auto thread_model = Component::IKVStore::THREAD_MODEL_SINGLE_PER_POOL;
#endif

template<typename T>
  struct type_number;

template<> struct type_number<char> { static constexpr uint64_t value = 2; };

namespace
{
  constexpr bool option_DEBUG = false;
  namespace type_num
  {
    constexpr uint64_t persist = 1U;
    constexpr uint64_t heap = 2U;
  }
}

#if USE_CC_HEAP == 1
using ALLOC_T = allocator_cc<char, Persister>;
#elif USE_CC_HEAP == 2
using ALLOC_T = allocator_co<char, Persister>;
#elif USE_CC_HEAP == 3
using ALLOC_T = allocator_rc<char, Persister>;
#else /* USE_CC_HEAP */
using ALLOC_T = allocator_pobj_cache_aligned<char>;
#endif /* USE_CC_HEAP */

using DEALLOC_T = typename ALLOC_T::deallocator_type;
using KEY_T = persist_fixed_string<char, DEALLOC_T>;
using MAPPED_T = persist_fixed_string<char, DEALLOC_T>;

struct pstr_hash
{
  using argument_type = KEY_T;
  using result_type = std::uint64_t;
  static result_type hf(const argument_type &s)
  {
    return CityHash64(s.data(), s.size());
  }
};

using HASHER_T = pstr_hash;

using allocator_segment_t = ALLOC_T::rebind<std::pair<const KEY_T, MAPPED_T>>::other;
using allocator_atomic_t = ALLOC_T::rebind<impl::mod_control>::other;

#if USE_CC_HEAP == 1
#elif USE_CC_HEAP == 2
#else
template<> struct type_number<impl::mod_control> { static constexpr std::uint64_t value = 4; };
#endif /* USE_CC_HEAP */

using table_t =
  table<
  KEY_T
  , MAPPED_T
  , HASHER_T
  , std::equal_to<KEY_T>
  , allocator_segment_t
  , hstore_shared_mutex
  >;

template<> struct type_number<table_t::value_type> { static constexpr std::uint64_t value = 5; };
template<> struct type_number<table_t::base::persist_data_t::bucket_aligned_t> { static constexpr uint64_t value = 6; };

using persist_data_t = typename impl::persist_data<allocator_segment_t, table_t::value_type>;

template <typename Handle, typename Allocator, typename Table>
  class session;
#if USE_PMEM
#include "hstore_pmem.h"
using session_t = session<hstore_pmem::open_pool_handle, ALLOC_T, table_t>;
#else
#include "hstore_nupm.h"
using session_t = session<hstore_nupm::open_pool_handle, ALLOC_T, table_t>;
#endif

/* globals */

thread_local std::set<tracked_pool *> tls_cache = {};

auto hstore::locate_session(const Component::IKVStore::pool_t pid) -> tracked_pool &
{
  return dynamic_cast<tracked_pool &>(this->locate_open_pool(pid));
}

auto hstore::locate_open_pool(const Component::IKVStore::pool_t pid) -> tracked_pool &
{
  auto *const s = reinterpret_cast<tracked_pool *>(pid);
  auto it = tls_cache.find(s);
  if ( it == tls_cache.end() )
  {
    std::unique_lock<std::mutex> sessions_lk(_pools_mutex);
    auto ps = _pools.find(s);
    if ( ps == _pools.end() )
    {
      throw API_exception(PREFIX "invalid pool identifier %p", __func__, s);
    }
    it = tls_cache.insert(ps->second.get()).first;
  }
  return **it;
}

auto hstore::move_pool(const Component::IKVStore::pool_t pid) -> std::unique_ptr<tracked_pool>
{
  auto *const s = reinterpret_cast<tracked_pool *>(pid);

  std::unique_lock<std::mutex> sessions_lk(_pools_mutex);
  auto ps = _pools.find(s);
  if ( ps == _pools.end() )
    {
      throw API_exception(PREFIX "invalid pool identifier %p", __func__, s);
    }

  tls_cache.erase(s);
  auto s2 = std::move(ps->second);
  _pools.erase(ps);
  return s2;
}

hstore::hstore(const std::string &owner, const std::string &name, std::unique_ptr<Devdax_manager> mgr_)
#if USE_PMEM
  : _pool_manager(std::make_shared<hstore_pmem>(owner, name, option_DEBUG))
#else
  : _pool_manager(std::make_shared<hstore_nupm>(owner, name, std::move(mgr_), option_DEBUG))
#endif
  , _pools_mutex{}
  , _pools{}
{
}

hstore::~hstore()
{
}

auto hstore::thread_safety() const -> status_t
{
  return thread_model;
}

auto hstore::create_pool(
                         const std::string & dir_,
                         const std::string & name_,
                         const std::size_t size_,
                         unsigned int /* flags */,
                         const uint64_t  expected_obj_count_) -> pool_t
{
  std::cerr << "create_pool " << dir_ << "/" << name_ << " size " << size_ << "\n";
  if ( option_DEBUG )
  {
    PLOG(PREFIX "dir=%s pool_name=%s", __func__, dir_.c_str(), name_.c_str());
  }
  {
    auto c = _pool_manager->pool_create_check(size_);
    if ( c != S_OK )  { return c; }
  }

  auto path = pool_path(dir_, name_);

  auto s = std::unique_ptr<session_t>(static_cast<session_t *>(_pool_manager->pool_create(path, size_, expected_obj_count_).release()));

  auto p = s.get();
  std::unique_lock<std::mutex> sessions_lk(_pools_mutex);
  _pools.emplace(p, std::move(s));

  return reinterpret_cast<IKVStore::pool_t>(p);
}

auto hstore::open_pool(const std::string &dir,
                       const std::string &name,
                       unsigned int /* flags */) -> pool_t
{
  auto path = pool_path(dir, name);
  auto s = _pool_manager->pool_open(path);
  auto p = static_cast<tracked_pool *>(s.get());
  std::unique_lock<std::mutex> sessions_lk(_pools_mutex);
  _pools.emplace(p, std::move(s));
  return reinterpret_cast<IKVStore::pool_t>(p);
}

void hstore::close_pool(const pool_t pid)
{
  std::string path;
  try
  {
    auto pool = move_pool(pid);
    if ( option_DEBUG )
    {
      PLOG(PREFIX "closed pool (%lx)", __func__, pid);
    }
  }
  catch ( const API_exception &e )
  {
    throw API_exception("%s in %s", e.cause(), __func__);
  }
  _pool_manager->pool_close_check(path);
}

void hstore::delete_pool(const std::string &dir, const std::string &name)
{
  auto path = pool_path(dir, name);

  _pool_manager->pool_delete(path);
  if ( option_DEBUG )
  {
    PLOG("pool deleted: %s/%s", dir.c_str(), name.c_str());
  }
}

void hstore::delete_pool(const pool_t pid)
try
{
  /* Not sure why a session would have to be open in order to erase a pool,
   * but the kvstore interface requires it.
   */
  pool_path pp;
  {
    auto pool = move_pool(pid);
    pp = pool->path();
  }
  delete_pool(pp.dir(), pp.name());
}
catch ( const API_exception &e )
{
  throw API_exception("%s in %s", e.cause(), __func__);
}

auto hstore::put(const pool_t pool,
                 const std::string &key,
                 const void * value,
                 const std::size_t value_len) -> status_t
{
  if ( option_DEBUG ) {
    PLOG(
         PREFIX "(key=%s) (value=%.*s)"
         , __func__
         , key.c_str()
         , int(value_len)
         , static_cast<const char*>(value)
         );
    assert(0 < value_len);
  }

  if(value == nullptr)
    throw std::invalid_argument("value argument is null");

  auto &session = dynamic_cast<session_t &>(locate_session(pool));

  auto cvalue = static_cast<const char *>(value);

  const auto i =
#if 1
    session.map().emplace(
                          std::piecewise_construct
                          , std::forward_as_tuple(key.begin(), key.end(), session.allocator())
                          , std::forward_as_tuple(cvalue, cvalue + value_len, session.allocator())
                          );
#else
    session.map().insert(
      table_t::value_type(
        table_t::key_type(key.begin(), key.end(), session.allocator())
	, table_t::mapped_type(cvalue, cvalue + value_len, session.allocator())
      )
    );
#endif
  return i.second ? S_OK : update_by_issue_41(pool, key, value, value_len,  i.first->second.data(), i.first->second.size());
}

auto hstore::update_by_issue_41(const pool_t pool,
                 const std::string &key,
                 const void * value,
                 const std::size_t value_len,
                 void * /* old_value */,
                 const std::size_t old_value_len
) -> status_t
try
{
  /* hstore issue 41: "a put should replace any existing k,v pairs that match. If the new put is a different size, then the object should be reallocated. If the new put is the same size, then it should be updated in place." */
  if ( value_len != old_value_len )
  {
    auto &session = dynamic_cast<session_t &>(locate_session(pool));
    auto p_key = KEY_T(key.begin(), key.end(), session.allocator());
    return session.enter_replace(p_key, value, value_len);
  }
  else {
    std::vector<std::unique_ptr<IKVStore::Operation>> v;
    v.emplace_back(std::make_unique<IKVStore::Operation_write>(0, value_len, value));
    std::vector<IKVStore::Operation *> v2;
    std::transform(v.begin(), v.end(), std::back_inserter(v2), [] (const auto &i) { return i.get(); });
    return this->atomic_update(
      pool
      , key
      , v2
      , false
    );
  }
}
catch ( std::exception & )
{
  return E_FAIL;
}


auto hstore::get_pool_regions(const pool_t pool, std::vector<::iovec>& out_regions) -> status_t
{
  auto &session = dynamic_cast<session_t &>(locate_session(pool));
  return _pool_manager->pool_get_regions(session.pool(), out_regions);
}

auto hstore::put_direct(const pool_t pool,
                        const std::string& key,
                        const void * value,
                        const std::size_t value_len,
                        memory_handle_t) -> status_t
{
  return put(pool, key, value, value_len);
}

auto hstore::get(const pool_t pool,
                 const std::string &key,
                 void*& out_value,
                 std::size_t& out_value_len) -> status_t
{
#if 0
  PLOG(PREFIX " get(%s)", __func__, key.c_str());
#endif
  try
    {
      const auto &session = dynamic_cast<const session_t &>(locate_session(pool));
      auto p_key = KEY_T(key.begin(), key.end(), session.allocator());
      auto &v = session.map().at(p_key);

      if(out_value == nullptr || out_value_len == 0) {
        out_value_len = v.size();
        out_value = malloc(out_value_len);
        if ( ! out_value )
          throw std::bad_alloc();
      }
      memcpy(out_value, v.data(), out_value_len);
      return S_OK;
    }
  catch ( std::out_of_range & )
    {
      return E_KEY_NOT_FOUND;
    }
  catch (...) {
    throw General_exception(PREFIX "failed unexpectedly", __func__);
  }
}

auto hstore::get_direct(const pool_t pool,
                        const std::string & key,
                        void* out_value,
                        std::size_t& out_value_len,
                        Component::IKVStore::memory_handle_t) -> status_t
  try {
    const auto &session = dynamic_cast<const session_t &>(locate_session(pool));
    auto p_key = KEY_T(key.begin(), key.end(), session.allocator());

    auto &v = session.map().at(p_key);

    auto value_len = v.size();
    if (out_value_len < value_len)
      {
        /* NOTE: it might be helpful to tell the caller how large a buffer we need,
         * but that dones not seem to be expected.
         */
        PWRN(PREFIX "failed; insufficient buffer", __func__);
        return E_INSUFFICIENT_BUFFER;
      }

    out_value_len = value_len;

    assert(out_value);

    /* memcpy for moment
     */
    memcpy(out_value, v.data(), out_value_len);
    if ( option_DEBUG )
      {
        PLOG(
             PREFIX "value_len=%lu value=(%s)", __func__
             , v.size()
             , static_cast<char*>(out_value)
             );
      }
    return S_OK;
  }
  catch ( const std::out_of_range & )
    {
      return E_KEY_NOT_FOUND;
    }
  catch(...) {
    throw General_exception("get_direct failed unexpectedly");
  }

#if 0
namespace
{
class lock
  : public Component::IKVStore::Opaque_key
  , public std::string
{
public:
  lock(const std::string &)
    : std::string(s)
    , Component::IKVStore::Opaque_key{}
  {}
};
}
#endif

namespace
{
bool try_lock(table_t &map, hstore::lock_type_t type, const KEY_T &p_key)
{
  return
    type == Component::IKVStore::STORE_LOCK_READ
    ? map.lock_shared(p_key)
    : map.lock_unique(p_key)
    ;
}
}

auto hstore::lock(const pool_t pool,
                  const std::string &key,
                  lock_type_t type,
                  void *& out_value,
                  std::size_t & out_value_len) -> key_t
{
  auto &session = dynamic_cast<session_t &>(locate_session(pool));
  const auto p_key = KEY_T(key.begin(), key.end(), session.allocator());

  try
    {
      MAPPED_T &val = session.map().at(p_key);
      if ( ! try_lock(session.map(), type, p_key) )
        {
          return KEY_NONE;
        }
      out_value = val.data();
      out_value_len = val.size();
    }
  catch ( std::out_of_range & )
    {
      /* if the key is not found, we create it and
         allocate value space equal in size to out_value_len
      */

      if ( option_DEBUG )
        {
          PLOG(PREFIX "allocating object %lu bytes", __func__, out_value_len);
        }

      auto r =
        session.map().emplace(
                              std::piecewise_construct
                              , std::forward_as_tuple(p_key)
                              , std::forward_as_tuple(out_value_len, session.allocator())
                              );

      if ( ! r.second )
        {
          return KEY_NONE;
        }

      out_value = r.first->second.data();
      out_value_len = r.first->second.size();
    }
  return reinterpret_cast<key_t>(new std::string(key));
}


auto hstore::unlock(const pool_t pool,
                    key_t key_) -> status_t
{
  std::string *key = reinterpret_cast<std::string *>(key_);

  if ( key )
    {
      try {
        auto &session = dynamic_cast<session_t &>(locate_session(pool));
        auto p_key = KEY_T(key->begin(), key->end(), session.allocator());

        session.map().unlock(p_key);
      }
      catch ( const std::out_of_range &e )
        {
          return E_KEY_NOT_FOUND;
        }
      catch(...) {
        throw General_exception(PREFIX "failed unexpectedly", __func__);
      }
      delete key;
    }

  return S_OK;
}

class maybe_lock
{
  table_t &_map;
  const KEY_T &_key;
  bool _taken;
public:
  maybe_lock(table_t &map_, const KEY_T &pkey_, bool take_)
    : _map(map_)
    , _key(pkey_)
    , _taken(false)
  {
    if ( take_ )
      {
        if( ! _map.lock_unique(_key) )
          {
            throw General_exception("unable to get write lock");
          }
        _taken = true;
      }
  }
  ~maybe_lock()
  {
    if ( _taken )
      {
        _map.unlock(_key); /* release lock */
      }
  }
};

auto hstore::apply(
                   const pool_t pool,
                   const std::string &key,
                   std::function<void(void*,std::size_t)> functor,
                   std::size_t object_size,
                   bool take_lock
                   ) -> status_t
{
  auto &session = dynamic_cast<session_t &>(locate_session(pool));
  MAPPED_T *val;
  auto p_key = KEY_T(key.begin(), key.end(), session.allocator());
  try
    {
      val = &session.map().at(p_key);
    }
  catch ( const std::out_of_range & )
    {
      /* if the key is not found, we create it and
         allocate value space equal in size to out_value_len
      */

      if ( option_DEBUG )
        {
          PLOG(PREFIX "allocating object %lu bytes", __func__, object_size);
        }

      auto r =
        session.map().emplace(
                              std::piecewise_construct
                              , std::forward_as_tuple(p_key)
                              , std::forward_as_tuple(object_size, session.allocator())
                              );
      if ( ! r.second )
        {
          return E_KEY_NOT_FOUND;
        }
      val = &(*r.first).second;
    }

  auto data = static_cast<char *>(val->data());
  auto data_len = val->size();

  maybe_lock m(session.map(), p_key, take_lock);

  functor(data, data_len);

  return S_OK;
}

auto hstore::erase(const pool_t pool,
                   const std::string &key
                   ) -> status_t
{
  try {
    auto &session = dynamic_cast<session_t &>(locate_session(pool));
    auto p_key = KEY_T(key.begin(), key.end(), session.allocator());
    return
      session.map().erase(p_key) == 0
      ? E_KEY_NOT_FOUND
      : S_OK
      ;
  }
  catch(...) {
    throw General_exception("hm_XXX_remove failed unexpectedly");
  }
}

std::size_t hstore::count(const pool_t pool)
{
  const auto &session = dynamic_cast<const session_t &>(locate_session(pool));
  return session.map().size();
}

void hstore::debug(const pool_t pool, const unsigned cmd, const uint64_t arg)
{
  switch ( cmd )
    {
    case 0:
      perishable::enable(bool(arg));
      break;
    case 1:
      perishable::reset(arg);
      break;
    case 2:
      {
        const auto &session = dynamic_cast<const session_t &>(locate_session(pool));
        table_t::size_type count = 0;
        /* bucket counter */
        for (
             auto n = session.map().bucket_count()
               ; n != 0
               ; --n
             )
          {
            auto last = session.map().end(n-1);
            for ( auto first = session.map().begin(n-1); first != last; ++first )
              {
                ++count;
              }
          }
        *reinterpret_cast<table_t::size_type *>(arg) = count;
      }
      break;
    default:
      break;
    };
#if 0
  auto &session = locate_session(pool);

  auto& root = session.root;
  auto& pop = session.pool();

  HM_CMD(pop, read_const_root(root)->map(), cmd, arg);
#endif
}

namespace
{
/* Return value not set. Ignored?? */
int _functor(
             const std::string &key
             , MAPPED_T &m
             , std::function
             <
             int(const std::string &key, const void *val, std::size_t val_len)
             > *lambda)
{
  assert(lambda);
  (*lambda)(key, m.data(), m.size());
  return 0;
}
}

auto hstore::map(
                 pool_t pool,
                 std::function
                 <
                 int(const std::string &key, const void *val, std::size_t val_len)
                 > function
                 ) -> status_t
{
  auto &session = dynamic_cast<session_t &>(locate_session(pool));

  for ( auto &mt : session.map() )
    {
      const auto &pstring = mt.first;
      std::string s(static_cast<const char *>(pstring.data()), pstring.size());
      _functor(s, mt.second, &function);
    }

  return S_OK;
}

auto hstore::atomic_update(
                           const pool_t pool
                           , const std::string& key
                           , const std::vector<Operation *> &op_vector
                           , const bool take_lock) -> status_t
  try
    {
      auto &session = dynamic_cast<session_t &>(locate_session(pool));

      auto p_key = KEY_T(key.begin(), key.end(), session.allocator());

      maybe_lock m(session.map(), p_key, take_lock);

      return session.enter_update(p_key, op_vector.begin(), op_vector.end());
    }
  catch ( std::exception & )
    {
      return E_FAIL;
    }

