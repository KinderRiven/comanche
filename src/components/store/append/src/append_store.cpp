/*
   Copyright [2017] [IBM Corporation]

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
#include <sstream>
#include <core/physical_memory.h>
#include "append_store.h"

using namespace Component;

/* thread local database handle, so we can open SQLite3 in multithread mode */
__thread sqlite3 *     g_tls_db;
std::vector<sqlite3 *> g_tls_db_vector;
std::mutex             g_tls_db_vector_lock;

class Semaphore {
public:
  Semaphore() { sem_init(&_sem,0,0); }
  inline void post() { sem_post(&_sem); }
  inline void wait() { sem_wait(&_sem); }
private:
  sem_t _sem;
};

struct __record_t
{
  int64_t lba;
  int64_t len;
};


static constexpr uint32_t APPEND_STORE_ITERATOR_MAGIC = 0x11110000;

struct __iterator_t
{
  uint32_t                magic;
  uint64_t                current_idx;
  uint64_t                exceeded_idx;
  std::vector<__record_t> record_vector;
};
  


static int callback(void *NotUsed, int argc, char **argv, char **azColName) {
   int i;
   for(i = 0; i<argc; i++) {
      printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
   }
   printf("\n");
   return 0;
}

static int print_callback(void *NotUsed, int argc, char **argv, char **azColName) {
   int i;
   for(i = 0; i < argc; i++) {
     //     printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
     if(i==0)
       printf("%s[%d] %s, ", NORMAL_BLUE, i, argv[i] ? argv[i] : "NULL");
     else
       printf("%s, ", argv[i] ? argv[i] : "NULL");
   }
   printf("\n%s", ESC_END);
   return 0;
}

// forward decls
//
static Component::IBlock_allocator *
create_block_allocator(Component::IPersistent_memory * pmem,
                       size_t n_blocks,
                       std::string name,
                       bool force_init);


sqlite3 * Append_store::db_handle()
{
  if(g_tls_db == nullptr) {
    int dbflags;
    if(_read_only)
      dbflags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
    else
      dbflags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;

    if(option_DEBUG)
      PLOG("Append-store: opening database (%s)",_db_filename.c_str());
    
    if(sqlite3_open_v2(_db_filename.c_str(),
                       &g_tls_db,
                       dbflags,
                       NULL) != SQLITE_OK) {
      throw General_exception("Append-store: failed to open sqlite3 db (%s)", _db_filename.c_str());
    }
    assert(g_tls_db);
      
    /* save in vector so we can release them all in destructor */
    g_tls_db_vector_lock.lock();
    //PLOG("saving db handle %p", g_tls_db);
    g_tls_db_vector.push_back(g_tls_db);
    g_tls_db_vector_lock.unlock();    
  }

  return g_tls_db;
}

Append_store::Append_store(const std::string owner,
                           const std::string name,
                           const std::string db_location,
                           Component::IBlock_device* block,
                           int flags)
  : _block(block),
    _hdr(block, owner, name, flags & FLAGS_FORMAT), /* initialize header object */
    _monitor([=]{ monitor_thread_entry(); }),
    _read_only(flags & FLAGS_READONLY)
{
  int rc;
  
  if(owner.empty() || name.empty() || block == nullptr)
    throw API_exception("bad Append_store constructor parameters");
 
  _lower_layer = _block;
  _block->add_ref();
  
  assert(_block);
  _block->get_volume_info(_vi);

  PLOG("Append-store: block device capacity=%lu max_dma_block=%ld",
       _vi.block_count, _vi.max_dma_len / _vi.block_size);

  _max_io_blocks = _vi.max_dma_len / _vi.block_size;
  _max_io_bytes  = _vi.max_dma_len;
  assert(_vi.max_dma_len % _vi.block_size == 0);
  
  /* database inititalization */
  _table_name = "appendstore";

  if(!db_location.empty())
    _db_filename = db_location + "/" + name + ".db";
  else
    _db_filename = "./" + name + ".db";

  PLOG("Append-store: db_filename=%s", _db_filename.c_str());
  
  if(flags & FLAGS_FORMAT) {
    std::remove(_db_filename.c_str());
  }
  
  if(option_DEBUG) {
    PLOG("Append-store: opening db %s", _db_filename.c_str());
  }
  
  db_handle();
  // if(sqlite3_open_v2(_db_filename.c_str(),
  //                    &_db,
  //                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
  //                    NULL) != SQLITE_OK) {
  //   throw General_exception("failed to open sqlite3 db (%s)", _db_filename.c_str());
  // }

  if(!_hdr.existing()) {
    /* create table if needed */
    execute_sql("CREATE TABLE IF NOT EXISTS appendstore (ID TEXT PRIMARY KEY NOT NULL, LBA INT8, NBLOCKS INT8, METADATA TEXT);");

    execute_sql("CREATE TABLE IF NOT EXISTS meta (KEY TEXT PRIMARY KEY NOT NULL, VALUE TEXT);");

    {
      std::stringstream ss;
      ss << "INSERT INTO meta VALUES('device_id','" << _vi.device_id << "');";
      execute_sql(ss.str());
    }

    {
      std::stringstream ss;
      ss << "INSERT INTO meta VALUES('volume_name','" << _vi.volume_name << "');";
      execute_sql(ss.str());
    }
  }
}

Append_store::~Append_store()
{
  //  show_db();

  g_tls_db_vector_lock.lock();
  for(auto& handle: g_tls_db_vector) {
    sqlite3_close(handle);
  }
  g_tls_db_vector_lock.unlock();


  _block->release_ref();

  _monitor.join();
}


void Append_store::monitor_thread_entry()
{
  if(!option_STATS) return;
  
  while(!_monitor_exit) {
    sleep(1);
    auto nbytes = stats.iterator_get_volume;
    stats.iterator_get_volume = 0;
    if(nbytes > 0)
      PLOG("read throughput: %lu MB/s", REDUCE_MB(nbytes));    
  }
}


uint64_t Append_store::insert_row(std::string& key, std::string& metadata, uint64_t lba, uint64_t length)
{
  std::stringstream sqlss;
  sqlss << "INSERT INTO " << _table_name << " VALUES ('" << key << "', " << lba << "," << length << ",'" << metadata << "')";
  execute_sql(sqlss.str());
  return 0;
}

bool Append_store::find_row(std::string& key, uint64_t& out_lba)
{
  std::stringstream sqlss;
  sqlss << "SELECT LBA FROM " << _table_name << " WHERE ID = '" << key << "';";
  std::string sql = sqlss.str();

  sqlite3_stmt * stmt;
  sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);
  int s = sqlite3_step(stmt);
  if(s == SQLITE_ROW) {
    out_lba = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return true;
  }
  else if(s == SQLITE_DONE) {
    sqlite3_finalize(stmt);
  }
  return false;
}

void Append_store::execute_sql(const std::string& sql, bool print_callback_flag)
{
  if(option_DEBUG) {
    PLOG("SQL:%s", sql.c_str());
  }

  char *zErrMsg = 0;
  unsigned remaining_retries = 1000000;

  while(remaining_retries > 0) {
    int rc;
    if(print_callback_flag) {
      if((rc=sqlite3_exec(db_handle(), sql.c_str(), print_callback, 0, &zErrMsg))==SQLITE_OK)
        break;
    }
    else {
      if((rc=sqlite3_exec(db_handle(), sql.c_str(), callback, 0, &zErrMsg))==SQLITE_OK)
        break;
    }
    if(rc != SQLITE_BUSY)
      throw General_exception("bad SQL statement (%s)", zErrMsg);
    usleep(1000);
    remaining_retries--;
  }
  if(remaining_retries == 0) {
    PERR("bad SQL statement (%s)", zErrMsg);
    assert(0);
    throw General_exception("bad SQL statement (%s)", zErrMsg);
  }
}

static void memory_free_cb(uint64_t gwid, void  *arg0, void* arg1)
{
  Append_store * pThis = reinterpret_cast<Append_store*>(arg0);
  io_buffer_t iob = reinterpret_cast<io_buffer_t>(arg1);
  pThis->phys_mem_allocator()->free_io_buffer(iob);
}

status_t Append_store::put(std::string key,
                           std::string metadata,
                           void * data,
                           size_t data_len,
                           int queue_id)
{
  assert(_block);
  assert(data_len > 0);

  if(_read_only) {
    assert(0);
    return E_INVAL;
  }

  char * p = static_cast<char *>(data);
  size_t n_blocks;
  lba_t start_lba;

  /* allocate contiguous segment of blocks */
  start_lba = _hdr.allocate(data_len, n_blocks); 

#ifdef __clang__
  static thread_local Semaphore sem;
#else
  static __thread Semaphore sem;
#endif

  Component::io_buffer_t iob;
  
  if(data) { /* if data == NULL we don't write out, just reserve */
    if(option_DEBUG)
      PLOG("[+] Append-store: append %ld bytes at block=%ld Used blocks=%ld/%ld", data_len,
           start_lba,
           start_lba+n_blocks,
           _vi.block_count); 

    iob = _phys_mem_allocator.allocate_io_buffer(round_up(data_len,_vi.block_size),
                                                      DMA_ALIGNMENT_BYTES,
                                                      NUMA_NODE_ANY);
    assert(iob);
  
    memcpy(_phys_mem_allocator.virt_addr(iob), data, data_len);
  
    /* issue aync */
    _block->async_write(iob,
                        0,
                        start_lba, /* append store header */
                        n_blocks,
                        queue_id,
                        [](uint64_t gwid, void* arg0, void* arg1)
                        {
                          ((Semaphore *)arg0)->post();
                        },
                        (void*) &sem);
  }

  /* write metadata */
  insert_row(key, metadata, start_lba, n_blocks);

  if(data) {
    /* wait for io to complete */
    sem.wait();
    _phys_mem_allocator.free_io_buffer(iob);
  }
  
  return S_OK;
}

status_t Append_store::put(std::string key,
                           std::string metadata,
                           Component::io_buffer_t iob,
                           size_t offset,
                           size_t data_len,
                           int queue_id)
{
  assert(_block);

  if(_read_only) {
    assert(0);
    return E_INVAL;
  }

  size_t n_blocks;
  lba_t start_lba = _hdr.allocate(data_len, n_blocks); /* allocate contiguous segment of blocks */

  if(option_DEBUG)
    PLOG("[+] Append-store: append %ld bytes. Used blocks=%ld/%ld", data_len,
         start_lba+n_blocks, _vi.block_count); 

#ifdef __clang__
  static thread_local Semaphore sem;
#else  
  static __thread Semaphore sem;
#endif

  /* issue aync */
  _block->async_write(iob,
                      offset,
                      start_lba, /* append store header */
                      n_blocks,
                      queue_id,
                      [](uint64_t gwid, void* arg0, void* arg1)
                      {
                        ((Semaphore *)arg0)->post();
                      },
                      (void*) &sem);

  /* write metadata */
  insert_row(key, metadata, start_lba, n_blocks);

  /* wait for io to complete */
  sem.wait();

  return S_OK;
}

IStore::iterator_t Append_store::open_iterator(std::string expr,
                                               unsigned long flags)
{
  __iterator_t *iter = new __iterator_t;
  assert(iter);

  iter->current_idx = 0;
  iter->magic = APPEND_STORE_ITERATOR_MAGIC;
  
  std::stringstream sqlss;

  if(flags & FLAGS_ITERATE_ALL)
    sqlss << "SELECT LBA,NBLOCKS FROM " << _table_name << ";";
  else
    sqlss << "SELECT LBA,NBLOCKS FROM " << _table_name << " WHERE " << expr << " ;";
  
  std::string sql = sqlss.str();

  sqlite3_stmt * stmt;
  sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);
  int s;
  while((s = sqlite3_step(stmt)) != SQLITE_DONE) {

    if(s == SQLITE_ERROR || s == SQLITE_MISUSE)
      throw API_exception("failed to open iterator: SQL statement failed (%s)", sql.c_str());
    
    iter->record_vector.push_back({sqlite3_column_int64(stmt, 0),sqlite3_column_int64(stmt, 1)});
  }
  sqlite3_finalize(stmt);
  iter->exceeded_idx = iter->record_vector.size();

  if(option_DEBUG) 
    PLOG("opened expr iterator (%p): records=%ld",
         iter, iter->exceeded_idx);

  return iter;
}

IStore::iterator_t Append_store::open_iterator(uint64_t rowid_start,
                                               uint64_t rowid_end,
                                               unsigned long flags)
{
  __iterator_t *iter = new __iterator_t;
  assert(iter);

  if(rowid_end < rowid_start)
    throw API_exception("open_iterator bad params");
  
  iter->current_idx = 0;
  iter->magic = APPEND_STORE_ITERATOR_MAGIC;
  
  std::stringstream sqlss;
  sqlss << "SELECT LBA,NBLOCKS FROM " << _table_name << " WHERE ROWID >= " << rowid_start <<
    " AND ROWID <= " << rowid_end << ";";
  std::string sql = sqlss.str();

  sqlite3_stmt * stmt;
  sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);
  int s;
  while((s = sqlite3_step(stmt)) != SQLITE_DONE) {

    if(s == SQLITE_ERROR || s == SQLITE_MISUSE)
      throw API_exception("failed to open iterator: SQL statement failed (%s)", sql.c_str());
    
    iter->record_vector.push_back({sqlite3_column_int64(stmt, 0),sqlite3_column_int64(stmt, 1)});
  }
  sqlite3_finalize(stmt);
  iter->exceeded_idx = iter->record_vector.size();

  if(option_DEBUG) 
    PLOG("opened iterator (%p): records=%ld",
         iter, iter->exceeded_idx);
    
  return iter;
}

size_t Append_store::iterator_record_count(iterator_t iter)
{
  auto iteritf = static_cast<__iterator_t*>(iter);
  assert(iteritf != nullptr);
  return iteritf->record_vector.size();
}

size_t Append_store::iterator_data_size(iterator_t iter)
{
  auto iteritf = static_cast<__iterator_t*>(iter);

  size_t total_blocks = 0;
  for(auto& i: iteritf->record_vector) {
    total_blocks += i.len;
  }
  return _vi.block_size * total_blocks;
}

size_t Append_store::iterator_next_record_size(iterator_t iter)
{
  auto i = static_cast<__iterator_t*>(iter);

  if(i->current_idx == i->exceeded_idx)
    return 0;

  auto record = i->record_vector[i->current_idx];
  return record.len * _vi.block_size;
}


/** 
 * Close iterator
 * 
 * @param iter Iterator
 */
void Append_store::close_iterator(IStore::iterator_t iter)
{
  auto iteritf = static_cast<__iterator_t*>(iter);
  assert(iteritf != nullptr);
    
  delete iteritf;
}


size_t Append_store::iterator_get(iterator_t iter,
                                  Component::io_buffer_t* iob,
                                  size_t offset,
                                  int queue_id)
{
  assert(iob);
  auto i = static_cast<__iterator_t*>(iter);
  assert(i);

  if(unlikely(i->magic != APPEND_STORE_ITERATOR_MAGIC))
    throw API_exception("Append_store::iterator_get - bad iterator (magic mismatch)");
  
  if(i->current_idx == i->exceeded_idx)
    return 0;

  auto& record = i->record_vector[i->current_idx];

  if(option_DEBUG) {
    PLOG("Append_store::iterator_get lba=%lu len=%lu", record.lba, record.len);
  }

  if(unlikely(get_size(*iob) < record.len))
    throw API_exception("insufficient space in iob for record len");

  _lower_layer->read(*iob,
                     offset,
                     record.lba, /* add one for store header */
                     record.len,
                     queue_id);

  i->current_idx++;
  size_t n_bytes = record.len * _vi.block_size;
  stats.iterator_get_volume += n_bytes;
  return n_bytes;
}


size_t Append_store::iterator_get(iterator_t iter,
                                  Component::io_buffer_t& iob,
                                  int queue_id)
{
  auto i = static_cast<__iterator_t*>(iter);
  assert(i);

  if(i->magic != APPEND_STORE_ITERATOR_MAGIC)
    throw API_exception("Append_store::iterator_get - bad iterator (magic incorrect)");

  if(i->current_idx == i->exceeded_idx) {
    return 0;
  }

  auto record = i->record_vector[i->current_idx];
  auto record_size = record.len * _vi.block_size;

  /* allocate IO buffer */
  iob = _lower_layer->allocate_io_buffer(record_size, KB(4), Component::NUMA_NODE_ANY);
  
  this->iterator_get(iter, &iob, 0, queue_id);
  
  return record_size;
}


void Append_store::split_iterator(iterator_t iter,
                                  size_t ways,
                                  std::vector<iterator_t>& out_iter_vector)
{
  auto source_iter = static_cast<__iterator_t*>(iter);

  if(ways < 2 || source_iter == nullptr)
    throw API_exception("invalid parameter to split_iterator");

  out_iter_vector.clear();

  std::vector<__iterator_t*> iv;
  
  for(unsigned i=0;i<ways;i++) {
    auto m = new __iterator_t;
    m->current_idx = 0;
    m->magic = APPEND_STORE_ITERATOR_MAGIC;
    iv.push_back(m);
  }
    
  for(unsigned j=0;j<source_iter->record_vector.size();j++)
    iv[j % ways]->record_vector.push_back(source_iter->record_vector[j]);

  for(auto& e: iv) {
    e->exceeded_idx = e->record_vector.size();
    out_iter_vector.push_back(static_cast<void*>(e));
  }
  
  delete source_iter;
}

void Append_store::reset_iterator(iterator_t iter)
{
  auto i = static_cast<__iterator_t*>(iter);
  i->current_idx = 0;
}

size_t Append_store::fetch_metadata(const std::string filter_expr,
                                    std::vector<std::pair<std::string,std::string> >& out_metadata)
{
  PLOG("Append-store::fetch_metadata");
  
  std::stringstream sqlss;
  int rc = 0;
  
  sqlss << "SELECT ID,METADATA FROM " << _table_name;
  if(!filter_expr.empty())
    sqlss << " WHERE " << filter_expr;
  sqlss << ";";

  std::string sql = sqlss.str();
  PLOG("SQL:(%s)", sql.c_str());
  sqlite3_stmt * stmt;
  rc = sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);
  assert(rc == SQLITE_OK);

  while(sqlite3_step(stmt) != SQLITE_DONE) {
    auto key = sqlite3_column_text(stmt, 0);
    auto metadata = sqlite3_column_text(stmt, 1);
    std::pair<std::string,std::string> p;
    p.first = (char *) key;
    p.second = (char *) metadata;
    out_metadata.push_back(p);
    rc++;
  }

  sqlite3_finalize(stmt);
  return rc;
}

uint64_t Append_store::check_path(const std::string path)
{
  std::stringstream sqlss;
  int rc = 0;
  
  sqlss << "SELECT rowid FROM " << _table_name;
  sqlss << " WHERE ID='" << path << "';";

  std::string sql = sqlss.str();
  PLOG("SQL:(%s)", sql.c_str());
  sqlite3_stmt * stmt;
  rc = sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);

  if(sqlite3_step(stmt) != SQLITE_DONE) {
    return sqlite3_column_int64(stmt, 0);
  }
  else return 0;
}


status_t Append_store::flush()
{
  if(_read_only) {
    assert(0);
    return E_INVAL;
  }

  _block->check_completion(0,0); /* wait for all pending */
  return S_OK;
}


void Append_store::dump_info()
{
  _hdr.dump_info();

  std::stringstream sqlss;
  sqlss << "SELECT * FROM " << _table_name << " LIMIT(100);";
  std::string sql = sqlss.str();

  /* dump keys */
  sqlite3_stmt * stmt;
  int s;
  s = sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);
  if(s != SQLITE_OK) {
    PERR("sqlite3 error:%d %s", s, sqlite3_errmsg(db_handle()));
    return;
  }

  while((s = sqlite3_step(stmt)) != SQLITE_DONE) {
    if(s != SQLITE_ROW) {
      PERR("sqlite3 error:%d", s);
      break;
    }
    auto key = sqlite3_column_text(stmt, 0);
    auto start_lba = sqlite3_column_int64(stmt, 1);
    auto len = sqlite3_column_int64(stmt, 2);
    PLOG("start_lba=%lld len=%lld key: %s ", start_lba, len, key);
  }
  PLOG("...");
  sqlite3_finalize(stmt);
}

void Append_store::show_db()
{
  assert(db_handle());
  std::stringstream sqlss;
  sqlss << "SELECT * FROM " << _table_name << ";";
  execute_sql(sqlss.str(), true);
}

size_t Append_store::get_record_count()
{
  assert(db_handle());

  std::stringstream sqlss;
  sqlss << "SELECT MAX(ROWID) FROM " << _table_name << ";";
  std::string sql = sqlss.str();
  sqlite3_stmt * stmt;
  int s;
  
  sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);

  sqlite3_int64 max_row_id;
  while((s = sqlite3_step(stmt)) != SQLITE_DONE) {
    
    if(s == SQLITE_ERROR || s == SQLITE_MISUSE)
      throw API_exception("Append_store::get_record_count: failed to execute SQL statement (%s)", sql.c_str());

    max_row_id = sqlite3_column_int64(stmt, 0);
  }

  sqlite3_finalize(stmt);

  return max_row_id;
}

status_t Append_store::get(uint64_t rowid,
                           Component::io_buffer_t iob,
                           size_t offset,
                           int queue_id)
{
  std::stringstream sqlss;
  sqlss << "SELECT * FROM " << _table_name << " WHERE ROWID=" << rowid << ";";
  std::string sql = sqlss.str();

  if(offset % _vi.block_size)
    throw API_exception("offset must be aligned with block size");
  
  sqlite3_stmt * stmt;
  sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);
  int s = sqlite3_step(stmt);
  int64_t data_lba = sqlite3_column_int64(stmt, 1);
  int64_t data_len = sqlite3_column_int64(stmt, 2);
  sqlite3_finalize(stmt);
  if(option_DEBUG) {
    PLOG("get(rowid=%lu) --> lba=%ld len=%ld", rowid, data_lba, data_len);
  }

  if((_lower_layer->get_size(iob) - offset) < (data_len * _vi.block_size)) {
    PWRN("Append_store:get call with too smaller IO buffer");    
    return E_INSUFFICIENT_SPACE;
  }
  
  assert(data_len > 0);
  _lower_layer->read(iob,
                     offset,
                     data_lba,
                     data_len,
                     queue_id);

  return S_OK;
}

status_t Append_store::get(const std::string key,
                           Component::io_buffer_t iob,
                           size_t offset,
                           int queue_id)
{
  std::stringstream sqlss;
  sqlss << "SELECT LBA, NBLOCKS FROM " << _table_name << " WHERE ID='" << key << "';";
  std::string sql = sqlss.str();
  PLOG("%s",sql.c_str());

  if(offset % _vi.block_size)
    throw API_exception("offset must be aligned with block size");
  
  sqlite3_stmt * stmt;
  sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);
  int s = sqlite3_step(stmt);
  int64_t data_lba = sqlite3_column_int64(stmt, 0);
  int64_t data_len = sqlite3_column_int64(stmt, 1);
  sqlite3_finalize(stmt);
  if(option_DEBUG || 1) {
    PLOG("get(key=%s) --> lba=%ld len=%ld", key.c_str(), data_lba, data_len);
  }

  if((_lower_layer->get_size(iob) - offset) < (data_len * _vi.block_size)) {
    PWRN("Append_store:get call with too smaller (%lu KB) IO buffer", REDUCE_KB(_lower_layer->get_size(iob)));    
    return E_INSUFFICIENT_SPACE;
  }
  assert(data_len > 0);
  
  _lower_layer->read(iob,
                     offset,
                     data_lba,
                     data_len,
                     queue_id);

  return S_OK;
}



std::string Append_store::get_metadata(uint64_t rowid)
{
  std::stringstream sqlss;
  sqlss << "SELECT ID FROM " << _table_name << " WHERE ROWID=" << rowid << ";";
  std::string sql = sqlss.str();

  sqlite3_stmt * stmt;
  sqlite3_prepare_v2(db_handle(), sql.c_str(), sql.size(), &stmt, nullptr);
  int s = sqlite3_step(stmt);

  if(s == SQLITE_ERROR)
    throw API_exception("unable to get metadata for row %lu", rowid);
  std::string result;
  result = (char*) sqlite3_column_text(stmt, 0);
  sqlite3_finalize(stmt);

  return result;
}


/** 
 * Static functions
 * 
 */
static Component::IBlock_allocator *
create_block_allocator(Component::IPersistent_memory * pmem,
                       size_t n_blocks,
                       std::string name,
                       bool force_init)
{
  assert(pmem);
  
  IBase * comp = load_component("libcomanche-allocblock.so",
                                Component::block_allocator_factory);
  assert(comp);
  IBlock_allocator_factory * fact = static_cast<IBlock_allocator_factory *>
    (comp->query_interface(IBlock_allocator_factory::iid()));

  auto alloc = fact->open_allocator(pmem, n_blocks, name+"-blka", Component::NUMA_NODE_ANY, force_init);
  fact->release_ref();  
  assert(alloc);
  return alloc;
}

/** 
 * Factory entry point
 * 
 */
extern "C" void * factory_createInstance(Component::uuid_t& component_id)
{
  if(component_id == Append_store_factory::component_id()) {
    return static_cast<void*>(new Append_store_factory());
  }
  else return NULL;
}

