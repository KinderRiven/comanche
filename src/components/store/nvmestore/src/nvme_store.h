/*
 * (C) Copyright IBM Corporation 2018. All rights reserved.
 *
 */

/* 
 * Authors: 
 * 
 * Feng Li (fengggli@yahoo.com)
 */

#ifndef NVME_STORE_H_
#define NVME_STORE_H_

#include <libpmemobj.h>

#include <atomic>
#include <unordered_map>
#include <pthread.h>
#include <common/rwlock.h>
#include <common/types.h>

#include <api/kvstore_itf.h>

#include "state_map.h"

class State_map;

//static constexpr char PMEM_PATH_ALLOC[] = "/mnt/pmem0/pool/0/"; // the pool for allocation info

POBJ_LAYOUT_BEGIN(nvme_store);
POBJ_LAYOUT_ROOT(nvme_store, struct store_root_t);
POBJ_LAYOUT_TOID(nvme_store, struct block_range);
POBJ_LAYOUT_END(nvme_store);


/*
 * for block allocator
 */
typedef struct block_range{
  int lba_start;
  int size; // size in bytes
  void * handle; // handle to free this block
  //uint64_t last_tag; // tag for async block io
} block_range_t;

class NVME_store : public Component::IKVStore
{
  using io_buffer_t = uint64_t;
private:
  static constexpr bool option_DEBUG = true;
  static constexpr size_t BLOCK_SIZE = 4096; // TODO: this should be obtained by querying the block device
  static constexpr size_t CHUNK_SIZE_IN_BLOCKS= 8; // large IO will be splited into CHUNKs, 8*4k  seems gives optimal
  static constexpr size_t DEFAULT_IO_MEM_SIZE= MB(8); // initial IO memory size in bytes
  std::unordered_map<pool_t, std::atomic<size_t>> _cnt_elem_map;
  std::string _pm_path; 
  Component::IBlock_device *_blk_dev;
  Component::IBlock_allocator *_blk_alloc;

  State_map _sm; // map control

  /* type of block io*/
  enum {
    BLOCK_IO_NOP = 0,
    BLOCK_IO_READ = 1,
    BLOCK_IO_WRITE = 2,
  };

public:
  /** 
   * Constructor
   * 
   * @param owner
   * @param name
   * @param pci pci address of the Nvme
   *   The "pci address" is in Bus:Device.Function (BDF) form with Bus and Device zero-padded to 2 digits each, e.g. 86:00.0
   */
  NVME_store(const std::string& owner,
             const std::string& name,
             const std::string& pci,
             const std::string& pm_path);

  /** 
   * Destructor
   * 
   */
  virtual ~NVME_store();

  /** 
   * Component/interface management
   */
  DECLARE_VERSION(0.1);
  DECLARE_COMPONENT_UUID(0x59564581,0x9e1b,0x4811,0xbdb2,0x19,0x57,0xa0,0xa6,0x84,0x57);

  void * query_interface(Component::uuid_t& itf_uuid) override {
    if(itf_uuid == Component::IKVStore::iid()) {
      return (void *) static_cast<Component::IKVStore*>(this);
    }
    else return NULL; // we don't support this interface
  }

  void unload() override {
    delete this;
  }

public:

  /* IKVStore */
  virtual int thread_safety() const override { return THREAD_MODEL_SINGLE_PER_POOL; }


  virtual pool_t create_pool(const std::string& path,
                             const std::string& name,
                             const size_t size,
                             unsigned int flags,
                             uint64_t expected_obj_count = 0
                             ) override;

  virtual pool_t open_pool(const std::string& path,
                           const std::string& name,
                           unsigned int flags) override;


  virtual void delete_pool(const pool_t pid) override;

  virtual void close_pool(const pool_t pid) override;

  virtual status_t put(const pool_t pool,
                       const std::string& key,
                       const void * value,
                       const size_t value_len) override;

  virtual status_t get(const pool_t pool,
                  const std::string& key,
                  void*& out_value,
                  size_t& out_value_len) override;

  virtual status_t get_direct(const pool_t pool,
                              const std::string& key,
                              void* out_value,
                              size_t& out_value_len,
                              Component::IKVStore::memory_handle_t handle) override;

  virtual IKVStore::memory_handle_t register_direct_memory(void * vaddr, size_t len) override;

  virtual IKVStore::key_t lock(const pool_t pool,
                               const std::string& key,
                               lock_type_t type,
                               void*& out_value,
                               size_t& out_value_len) override;

  virtual status_t unlock(const pool_t pool,
                          key_t key_hash) override;

  virtual status_t apply(const pool_t pool,
                         const std::string& key,
                         std::function<void(void*,const size_t)> functor,
                         size_t object_size,
                         bool take_lock = true) override;

  virtual status_t erase(const pool_t pool,
                         const std::string& key) override;

  virtual size_t count(const pool_t pool) override { return _cnt_elem_map[pool]; }

  virtual void debug(const pool_t pool, unsigned cmd, uint64_t arg) { }

private:

  /*
   * open the block device, reuse if it exists already
   *
   * @param pci in pci address of the nvme
   *   The "pci address" is in Bus:Device.Function (BDF) form with Bus and Device zero-padded to 2 digits each, e.g. 86:00.0
   *   The domain is implicitly 0000.
   * @param block out reference of block device
   *
   * @return S_OK if success
   */
  status_t open_block_device(const std::string &pci, Component::IBlock_device* &block);

  /*
   * open an allocator for block device, reuse if it exsits already
   */

  status_t open_block_allocator(Component::IBlock_device* block, Component::IBlock_allocator* &alloc);

  /*
   * Issue block device io to one block device
   *
   * @param block block device
   * @param type read/write
   * @param mem io memory
   * @param lba block address
   * @param nr_io_blocks block to be operated on, all the blocks should fit in the IO memory
   *
   * This call itself is synchronous
   */
  status_t  do_block_io(Component::IBlock_device * block,
                           int type,
                           io_buffer_t mem,
                           uint64_t lba,
                           size_t nr_io_blocks);
};


class NVME_store_factory : public Component::IKVStore_factory
{
public:

  /** 
   * Component/interface management
   * 
   */
  DECLARE_VERSION(0.1);
  DECLARE_COMPONENT_UUID(0xfac64581,0x1993,0x4811,0xbdb2,0x19,0x57,0xa0,0xa6,0x84,0x57);

  void * query_interface(Component::uuid_t& itf_uuid) override;

  void unload() override;

  /*
   *   "pci" is in Bus:Device.Function (BDF) form. Bus and Device must be zero-padded to 2 digits each, e.g. 86:00.0
   */
  virtual Component::IKVStore * create(const std::string& owner,
                                       const std::string& name,
                                       const std::string& pci)
                                       override;

  virtual Component::IKVStore * create(unsigned,
                                       const std::string& owner,
                                       const std::string& name,
                                       const std::string& pci)
                                       override;
};

#endif
