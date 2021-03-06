/*
   Copyright [2017,2018,2019] [IBM Corporation]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/


#ifndef __API_KVSTORE_ITF__
#define __API_KVSTORE_ITF__

#include <sys/uio.h> /* iovec */

#include <cstdlib>
#include <functional>
#include <vector>
#include <assert.h>

#include <api/components.h>
#include <api/block_itf.h>
#include <api/block_allocator_itf.h>

namespace Component
{

/** 
 * Key-value interface
 */
class IKVStore : public Component::IBase
{
public:
  // clang-format off
  DECLARE_INTERFACE_UUID(0x62f4829f,0x0405,0x4c19,0x9898,0xa3,0xae,0x21,0x5a,0x3e,0xe8);
  // clang-format on
  
private:
  struct Opaque_memory_region {
    virtual ~Opaque_memory_region() {}
  };

public:
  using pool_t          = uint64_t;
  using memory_handle_t = Opaque_memory_region *;
  
  struct Opaque_key { /* base for implementation lock/key container */
    virtual ~Opaque_key() {} /* destruction invokes derived destructor */
  };
  
  using key_t           = Opaque_key *;

  static constexpr memory_handle_t HANDLE_NONE = nullptr;
  static constexpr key_t KEY_NONE = nullptr;

  enum {
    THREAD_MODEL_UNSAFE,
    THREAD_MODEL_SINGLE_PER_POOL,
    THREAD_MODEL_RWLOCK_PER_POOL,
    THREAD_MODEL_MULTI_PER_POOL,
  };

  enum {
    FLAGS_READ_ONLY = 1,
    FLAGS_SET_SIZE = 2,
    FLAGS_CREATE_ONLY = 3,
  };

  enum {
    POOL_ERROR = 0,
  };

  enum class Op_type {
    WRITE, /* copy bytes into memory region */
    ZERO, /* zero the memory region */
    INCREMENT_UINT64,
    CAS_UINT64,
  };

  class Operation
  {
    Op_type _type;
    size_t _offset;
  protected:
    Operation(Op_type type, size_t offset)
      : _type(type)
      , _offset(offset)
    {}
  public:
    Op_type type() const noexcept { return _type; }
    size_t offset() const  noexcept{ return _offset; }
  };

  class Operation_sized
    : public Operation
  {
    size_t _len;
  protected:
    Operation_sized(Op_type type, size_t offset_, size_t len)
      : Operation(type, offset_)
      , _len(len)
    {}
  public:
    size_t size() const noexcept { return _len; }
  };

  class Operation_write
    : public Operation_sized
  {
    const void *_data;
  public:
    Operation_write(size_t offset, size_t len, const void *data)
      :  Operation_sized(Op_type::WRITE, offset, len)
      , _data(data)
    {}
    const void * data() const noexcept { return _data; }
  };

  typedef enum {
    STORE_LOCK_READ=1,
    STORE_LOCK_WRITE=2,
  } lock_type_t;

  enum {
    S_OK = 0,
    S_MORE = 1,
    E_FAIL = -1,
    E_KEY_EXISTS = -2,
    E_KEY_NOT_FOUND = -3,
    E_POOL_NOT_FOUND = -4,
    E_NOT_SUPPORTED = -5,
    E_ALREADY_EXISTS = -6,
    E_TOO_LARGE = -7,
    E_BAD_PARAM = -8,
    E_BAD_ALIGNMENT = -9,
    E_INSUFFICIENT_BUFFER = -10,
    E_BAD_OFFSET = -11,
  };

  /** 
   * Determine thread safety of the component
   * 
   * 
   * @return THREAD_MODEL_XXXX
   */
  virtual int thread_safety() const = 0;

  /** 
   * Create an object pool
   * 
   * @param path Path of the persistent memory (e.g., /mnt/pmem0/ )
   * @param name Name of object pool
   * @param size Size of object pool in bytes
   * @param flags Creation flags
   * @param
   * 
   * @return Pool handle
   */
  virtual pool_t create_pool(const std::string& path,
                             const std::string& name,
                             const size_t size,
                             unsigned int flags = 0,
                             uint64_t expected_obj_count = 0) = 0;

  /** 
   * Open an existing pool
   * 
   * @param path Path of persistent memory (e.g., /mnt/pmem0/ )
   * @param name Name of object pool
   * @param flags Open flags e.g., FLAGS_READ_ONLY
   * 
   * @return Pool handle
   */
  virtual pool_t open_pool(const std::string& path,
                           const std::string& name,
                           unsigned int flags = 0) = 0;

  /** 
   * Close pool handle
   * 
   * @param pool Pool handle
   */
  virtual void close_pool(const pool_t pool) = 0;

  /** 
   * Close and delete an existing pool
   * 
   * @param pool Pool handle
   */
  virtual void delete_pool(const pool_t pool)  = 0;

  /** 
   * Close and delete an existing pool
   * 
   * @param path Path of persistent memory (e.g., /mnt/pmem0/ )
   * @param name Name of object pool
   */
  virtual void delete_pool(const std::string &path, const std::string &name) {
    delete_pool(open_pool(path, name));
  }

  /** 
   * Get mapped memory regions for pool
   * 
   * @param pool Pool handle
   * @param out_regions Mapped memory regions
   *
   * @return S_OK on success.  Components that do not support this return E_NOT_SUPPORTED.
   */  
  virtual status_t get_pool_regions(const pool_t pool,
                                    std::vector<::iovec>& out_regions) {
    return E_NOT_SUPPORTED;
  }

  /** 
   * Write or overwrite an object value. If there already exists a
   * object with matching key, then it should be replaced
   * (i.e. reallocated) or overwritten. 
   * 
   * @param pool Pool handle
   * @param key Object key
   * @param value Value data
   * @param value_len Size of value in bytes
   * 
   * @return S_OK or error code
   */
  virtual status_t put(const pool_t pool,
                       const std::string& key,
                       const void * value,
                       const size_t value_len) { return E_NOT_SUPPORTED; }

  /** 
   * Zero-copy put operation.  If there does not exist an object
   * with matching key, then an error E_KEY_EXISTS should be returned.
   * 
   * @param pool Pool handle
   * @param key Object key
   * @param key_len Key length in bytes
   * @param value Value
   * @param value_len Value length in bytes
   * @param handle Memory registration handle 
   *
   * @return S_OK or error code
   */
  virtual status_t put_direct(const pool_t pool,
                              const std::string& key,
                              const void * value,
                              const size_t value_len,
                              memory_handle_t handle = HANDLE_NONE) {
    return E_NOT_SUPPORTED;
  }

  /** 
   * Read an object value
   * 
   * @param pool Pool handle
   * @param key Object key
   * @param out_value Value data (if null, component will allocate memory)
   * @param out_value_len Size of value in bytes
   * 
   * @return S_OK or error code
   */
  virtual status_t get(const pool_t pool,
                       const std::string& key,
                       void*& out_value, /* release with free_memory() API */
                       size_t& out_value_len) = 0;


  /**
   * Read an object value directly into client-provided memory.
   *
   * @param pool Pool handle
   * @param key Object key
   * @param out_value Client provided buffer for value
   * @param out_value_len [in] size of value memory in bytes [out] size of value
   * @param handle Memory registration handle
   * 
   * @return S_OK, S_MORE if only a portion of value is read, E_BAD_ALIGNMENT on invalid alignment, or other error code
   */
  virtual status_t get_direct(const pool_t pool,
                              const std::string& key,
                              void* out_value,
                              size_t& out_value_len,
                              memory_handle_t handle = HANDLE_NONE) {
    return E_NOT_SUPPORTED;
  }
  

  /** 
   * Register memory for zero copy DMA
   * 
   * @param vaddr Appropriately aligned memory buffer
   * @param len Length of memory buffer in bytes
   * 
   * @return Memory handle or NULL on not supported.
   */
  virtual memory_handle_t register_direct_memory(void * vaddr, size_t len) { return nullptr; }


  /** 
   * Durict memory regions should be unregistered before the memory is released on the client side.
   * 
   * @param vaddr Address of region to unregister.
   * 
   * @return S_OK on success
   */
  virtual status_t unregister_direct_memory(memory_handle_t handle) { return E_NOT_SUPPORTED; }


  /** 
   * Take a lock on an object. If the object does not exist, create it with
   * value space according to out_value_len
   * 
   * @param pool Pool handle
   * @param key Key
   * @param type STORE_LOCK_READ | STORE_LOCK_WRITE
   * @param out_value [out] Pointer to data
   * @param out_value_len [in-out] Size of data in bytes
   * 
   * @return Handle to key for unlock or KEY_NONE if unsupported
   */
  virtual key_t lock(const pool_t pool,
                     const std::string& key,
                     lock_type_t type,
                     void*& out_value,
                     size_t& out_value_len) { return KEY_NONE; }

  /** 
   * Unlock an object
   * 
   * @param pool Pool handle
   * @param key_handle Handle (opaque) for key
   * 
   * @return S_OK or error code
   */
  virtual status_t unlock(const pool_t pool,
                          key_t key_handle) { return E_NOT_SUPPORTED; }

  /** 
   * Apply a functor to an object as a transaction
   * 
   * @param pool Pool handle
   * @param key Object key
   * @param functor Functor to apply to object
   * @param object_size Size of object if creation is needed
   * @param take_lock Set to true to implicitly take the lock (otherwise lock/unlock should be called explicitly)
   * 
   * @return S_OK or error code
   */
  virtual status_t apply(const pool_t pool,
                         const std::string& key,
                         std::function<void(void*,const size_t)> functor,
                         size_t object_size,
                         bool take_lock = true) { return E_NOT_SUPPORTED; }



  /** 
   * Update an existing value by applying a series of operations.
   * Together the set of operations make up an atomic transaction.
   * If the operation requires a result the operation type may provide
   * a method to accept the result. No operation currently requires
   * a result, but compare and swap probably would.
   * 
   * @param pool Pool handle
   * @param key Object key
   * @param op_vector Operation vector
   * @param take_lock Set to true for automatic locking of object
   * 
   * @return S_OK or error code
   */
  virtual status_t atomic_update(const pool_t pool,
                                 const std::string& key,
                                 const std::vector<Operation *> & op_vector,
                                 bool take_lock = true) { return E_NOT_SUPPORTED; }

  /** 
   * Erase an object
   * 
   * @param pool Pool handle
   * @param key Object key
   * 
   * @return S_OK or error code
   */
  virtual status_t erase(const pool_t pool,
                         const std::string& key)= 0;


  /** 
   * Return number of objects in the pool
   * 
   * @param pool Pool handle
   * 
   * @return Number of objects
   */
  virtual size_t count(const pool_t pool) = 0;


  /** 
   * Apply functor to all objects in the pool
   * 
   * @param pool Pool handle
   * @param function Functor
   * 
   * @return S_OK or error code
   */
  virtual status_t map(const pool_t pool,
                       std::function<int(const std::string& key,
                                         const void * value,
                                         const size_t value_len)> function) { return E_NOT_SUPPORTED; }

  /**
   * Free server-side allocated memory
   *
   * @param p Pointer to memory allocated through a get call
   */
  virtual void free_memory(void * p) { return ::free(p); }


  /** 
   * Perform control invocation on component
   * 
   * @param command String representation of command (component-interpreted)
   * 
   * @return S_OK on success or error otherwise
   */
  virtual status_t ioctl(const std::string& command) { return E_NOT_SUPPORTED; }


  /** 
   * Debug routine
   * 
   * @param pool Pool handle
   * @param cmd Debug command
   * @param arg Parameter for debug operation
   */
  virtual void debug(const pool_t pool, unsigned cmd, uint64_t arg) = 0;
};


class IKVStore_factory : public Component::IBase
{
public:
  // clang-format off
  DECLARE_INTERFACE_UUID(0xface829f,0x0405,0x4c19,0x9898,0xa3,0xae,0x21,0x5a,0x3e,0xe8);
  // clang-format on
  
  virtual IKVStore * create(const std::string& owner,
                            const std::string& param){
    throw API_exception("factory::create(owner,param) not implemented");
  };

  virtual IKVStore * create(const std::string& owner,
                            const std::string& param,
                            const std::string& param2){
    throw API_exception("factory::create(owner,param,param2) not implemented");
  }

  virtual IKVStore * create(unsigned debug_level,
                            const std::string& owner,
                            const std::string& param,
                            const std::string& param2){
    throw API_exception("factory::create(debug_level,owner,param,param2) not implemented");
  }


};


}


#endif
