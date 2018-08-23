/*
   Copyright [2018] [IBM Corporation]

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

/*
 * Authors:
 *
 */

#include "fabric_comm_grouped.h"

#include "async_req_record.h"
#include "fabric_generic_grouped.h"
#include "fabric_op_control.h" /* fi_cq_entry_t */
#include "fabric_runtime_error.h"

#include <sys/uio.h> /* struct iovec */
#include <cstdlib> /* getenv */
#include <iostream> /* cerr */

/**
 * Fabric/RDMA-based network component
 *
 */

/* Note: the info is owned by the caller, and must be copied if it is to be saved. */
Fabric_comm_grouped::Fabric_comm_grouped(Fabric_generic_grouped &conn_)
  : _conn( conn_ )
  , _m_completions{}
  , _completions{}
  , _stats{}
{
}

Fabric_comm_grouped::stats::~stats()
{
  if ( std::getenv("FABRIC_STATS") )
  {
    std::cerr << "Fabric_comm_grouped(" << this << ") ct " << ct_total << " defer " << defer_total << " redirect " << redirect_total << "\n";
  }
}

Fabric_comm_grouped::~Fabric_comm_grouped()
{
/* wait until all completions are reaped */
  _conn.forget_group(this);
}

/**
 * Asynchronously post a buffer to the connection
 *
 * @param connection Connection to send on
 * @param buffers Buffer vector (containing regions should be registered)
 *
 * @return Work (context) identifier
 */
void Fabric_comm_grouped::post_send(
  const ::iovec *first_
  , const ::iovec *last_
  , void **desc_
  , void *context_
)
{
  std::unique_ptr<async_req_record> gc{new async_req_record(this, context_)};
  _conn.post_send(first_, last_, desc_, &*gc);
  gc.release();
}

void Fabric_comm_grouped::post_send(
  const std::vector<::iovec>& buffers_
  , void *context_
)
{
  std::unique_ptr<async_req_record> gc{new async_req_record(this, context_)};
  _conn.post_send(&*buffers_.begin(), &*buffers_.end(), &*gc);
  gc.release();
}

/**
 * Asynchronously post a buffer to receive data
 *
 * @param connection Connection to post to
 * @param buffers Buffer vector (containing regions should be registered)
 *
 * @return Work (context) identifier
 */
void Fabric_comm_grouped::post_recv(
  const ::iovec *first_
  , const ::iovec *last_
  , void **desc_
  , void *context_
)
{
  std::unique_ptr<async_req_record> gc{new async_req_record(this, context_)};
  _conn.post_send(first_, last_, desc_, &*gc);
  gc.release();
}

void Fabric_comm_grouped::post_recv(
  const std::vector<::iovec>& buffers_
  , void *context_
)
{
  std::unique_ptr<async_req_record> gc{new async_req_record(this, context_)};
  _conn.post_recv(&*buffers_.begin(), &*buffers_.end(), &*gc);
  gc.release();
}

  /**
   * Post RDMA read operation
   *
   * @param connection Connection to read on
   * @param buffers Destination buffer vector
   * @param remote_addr Remote address
   * @param key Key for remote address
   * @param out_context
   *
   */
void Fabric_comm_grouped::post_read(
  const ::iovec *first_
  , const ::iovec *last_
  , void **desc_
  , uint64_t remote_addr_
  , uint64_t key_
  , void *context_
)
{
  /* provide a read buffer */
  std::unique_ptr<async_req_record> gc{new async_req_record(this, context_)};
  _conn.post_read(first_, last_, desc_, remote_addr_, key_, &*gc);
  gc.release();
}

void Fabric_comm_grouped::post_read(
  const std::vector<::iovec>& buffers_,
  uint64_t remote_addr_,
  uint64_t key_,
  void *context_
)
{
  /* provide a read buffer */
  std::unique_ptr<async_req_record> gc{new async_req_record(this, context_)};
  _conn.post_read(&*buffers_.begin(), &*buffers_.end(), remote_addr_, key_, &*gc);
  gc.release();
}

  /**
   * Post RDMA write operation
   *
   * @param connection Connection to write to
   * @param buffers Source buffer vector
   * @param remote_addr Remote address
   * @param key Key for remote address
   * @param out_context
   *
   */
void Fabric_comm_grouped::post_write(
  const ::iovec *first_
  , const ::iovec *last_
  , void **desc_
  , uint64_t remote_addr_
  , uint64_t key_
  , void *context_
)
{
  std::unique_ptr<async_req_record> gc{new async_req_record(this, context_)};
  _conn.post_write(first_, last_, desc_, remote_addr_, key_, &*gc);
  gc.release();
}

void Fabric_comm_grouped::post_write(
  const std::vector<::iovec>& buffers_,
  uint64_t remote_addr_,
  uint64_t key_,
  void *context_
)
{
  std::unique_ptr<async_req_record> gc{new async_req_record(this, context_)};
  _conn.post_write(&*buffers_.begin(), &*buffers_.end(), remote_addr_, key_, &*gc);
  gc.release();
}

  /**
   * Send message without completion
   *
   * @param connection Connection to inject on
   * @param buffers Buffer vector (containing regions should be registered)
   */
void Fabric_comm_grouped::inject_send(const ::iovec *first_, const ::iovec *last_)
{
  _conn.inject_send(first_, last_);
}
void Fabric_comm_grouped::inject_send(const std::vector<::iovec>& buffers_)
{
  _conn.inject_send(&*buffers_.begin(), &*buffers_.end());
}

void Fabric_comm_grouped::queue_completion(::status_t status_, const Fabric_op_control::fi_cq_entry_t &cq_entry_)
{
  std::lock_guard<std::mutex> k2{_m_completions};
  _completions.push(completion_t(cq_entry_, status_));
}

std::size_t Fabric_comm_grouped::process_or_queue_completion(const Fabric_op_control::fi_cq_entry_t &cq_entry_, const Component::IFabric_op_completer::complete_old &cb_, ::status_t status_)
{
  std::size_t ct_total = 0U;
  std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry_.op_context));
  if ( g_context->comm() == this )
  {
    cb_(g_context->context(), status_);
    ++ct_total;
  }
  else
  {
    _conn.queue_completion(g_context->comm(), status_, cq_entry_);
    ++_stats.redirect_total;
    g_context.release();
  }

  return ct_total;
}

std::size_t Fabric_comm_grouped::process_or_queue_completion(const Fabric_op_control::fi_cq_entry_t &cq_entry_, const Component::IFabric_op_completer::complete_definite &cb_, ::status_t status_)
{
  std::size_t ct_total = 0U;
  std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry_.op_context));
  if ( g_context->comm() == this )
  {
    cb_(g_context->context(), status_, cq_entry_.flags, cq_entry_.len, nullptr);
    ++ct_total;
  }
  else
  {
    _conn.queue_completion(g_context->comm(), status_, cq_entry_);
    ++_stats.redirect_total;
    g_context.release();
  }

  return ct_total;
}

std::size_t Fabric_comm_grouped::process_or_queue_completion(const Fabric_op_control::fi_cq_entry_t &cq_entry_, const Component::IFabric_op_completer::complete_tentative &cb_, ::status_t status_)
{
  std::size_t ct_total = 0U;
  std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry_.op_context));
  if ( g_context->comm() == this && cb_(g_context->context(), status_, cq_entry_.flags, cq_entry_.len, nullptr) == cb_acceptance::ACCEPT )
  {
    ++ct_total;
  }
  else
  {
    _conn.queue_completion(g_context->comm(), status_, cq_entry_);
    ++(g_context->comm() == this ? _stats.defer_total : _stats.redirect_total);
    g_context.release();
  }

  return ct_total;
}

std::size_t Fabric_comm_grouped::process_or_queue_completion(const Fabric_op_control::fi_cq_entry_t &cq_entry_, const Component::IFabric_op_completer::complete_param_definite &cb_, ::status_t status_, void *cb_param_)
{
  std::size_t ct_total = 0U;
  std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry_.op_context));
  if ( g_context->comm() == this )
  {
    cb_(g_context->context(), status_, cq_entry_.flags, cq_entry_.len, nullptr, cb_param_);
    ++ct_total;
  }
  else
  {
    _conn.queue_completion(g_context->comm(), status_, cq_entry_);
    ++_stats.redirect_total;
    g_context.release();
  }

  return ct_total;
}

std::size_t Fabric_comm_grouped::process_or_queue_completion(const Fabric_op_control::fi_cq_entry_t &cq_entry_, const Component::IFabric_op_completer::complete_param_tentative &cb_, ::status_t status_, void *cb_param_)
{
  std::size_t ct_total = 0U;
  std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry_.op_context));
  if ( g_context->comm() == this && cb_(g_context->context(), status_, cq_entry_.flags, cq_entry_.len, nullptr, cb_param_) == cb_acceptance::ACCEPT )
  {
    ++ct_total;
  }
  else
  {
    _conn.queue_completion(g_context->comm(), status_, cq_entry_);
    ++(g_context->comm() == this ? _stats.defer_total : _stats.redirect_total);
    g_context.release();
  }

  return ct_total;
}

std::size_t Fabric_comm_grouped::process_cq_comp_err(const Component::IFabric_op_completer::complete_old &cb_)
{
  /* ERROR: the error context is not necessarily the expected context, and therefore may not be an async_req_record */
  const ::fi_cq_err_entry e{_conn.get_cq_comp_err()};
  const Fabric_op_control::fi_cq_entry_t err_entry{e.op_context, e.flags, e.len, e.buf, e.data};
  return process_or_queue_completion(err_entry, cb_, E_FAIL);
}

std::size_t Fabric_comm_grouped::process_cq_comp_err(const Component::IFabric_op_completer::complete_definite &cb_)
{
  /* ERROR: the error context is not necessarily the expected context, and therefore may not be an async_req_record */
  const ::fi_cq_err_entry e{_conn.get_cq_comp_err()};
  const Fabric_op_control::fi_cq_entry_t err_entry{e.op_context, e.flags, e.len, e.buf, e.data};
  return process_or_queue_completion(err_entry, cb_, E_FAIL);
}

std::size_t Fabric_comm_grouped::process_cq_comp_err(const Component::IFabric_op_completer::complete_tentative &cb_)
{
  /* ERROR: the error context is not necessarily the expected context, and therefore may not be an async_req_record */
  const ::fi_cq_err_entry e{_conn.get_cq_comp_err()};
  const Fabric_op_control::fi_cq_entry_t err_entry{e.op_context, e.flags, e.len, e.buf, e.data};
  return process_or_queue_completion(err_entry, cb_, E_FAIL);
}

std::size_t Fabric_comm_grouped::process_cq_comp_err(const Component::IFabric_op_completer::complete_param_definite &cb_, void *cb_param_)
{
  /* ERROR: the error context is not necessarily the expected context, and therefore may not be an async_req_record */
  const ::fi_cq_err_entry e{_conn.get_cq_comp_err()};
  const Fabric_op_control::fi_cq_entry_t err_entry{e.op_context, e.flags, e.len, e.buf, e.data};
  return process_or_queue_completion(err_entry, cb_, E_FAIL, cb_param_);
}

std::size_t Fabric_comm_grouped::process_cq_comp_err(const Component::IFabric_op_completer::complete_param_tentative &cb_, void *cb_param_)
{
  /* ERROR: the error context is not necessarily the expected context, and therefore may not be an async_req_record */
  const ::fi_cq_err_entry e{_conn.get_cq_comp_err()};
  const Fabric_op_control::fi_cq_entry_t err_entry{e.op_context, e.flags, e.len, e.buf, e.data};
  return process_or_queue_completion(err_entry, cb_, E_FAIL, cb_param_);
}

  /**
   * Poll completions (e.g., completions)
   *
   * @param cb_ (context_t, ::status_t status, void* error_data)
   *
   * @return Number of completions processed
   */

std::size_t Fabric_comm_grouped::drain_old_completions(const Component::IFabric_op_completer::complete_old &cb_)
{
  std::size_t ct_total = 0U;
  std::unique_lock<std::mutex> k{_m_completions};
  while ( ! _completions.empty() )
  {
    auto c = _completions.front();
    _completions.pop();
    k.unlock();
    auto &cq_entry{std::get<0>(c)};
    std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry.op_context));
    cb_(g_context->context(), std::get<1>(c));
    ++ct_total;
    k.lock();
  }
  return ct_total;
}

std::size_t Fabric_comm_grouped::drain_old_completions(const Component::IFabric_op_completer::complete_param_definite &cb_, void *cb_param_)
{
  std::size_t ct_total = 0U;
  std::unique_lock<std::mutex> k{_m_completions};
  while ( ! _completions.empty() )
  {
    auto c = _completions.front();
    _completions.pop();
    k.unlock();
    auto &cq_entry{std::get<0>(c)};
    std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry.op_context));
    cb_(g_context->context(), std::get<1>(c), cq_entry.flags, cq_entry.len, nullptr, cb_param_);
    ++ct_total;
    k.lock();
  }
  return ct_total;
}

std::size_t Fabric_comm_grouped::drain_old_completions(const Component::IFabric_op_completer::complete_param_tentative &cb_, void *cb_param_)
{
  std::size_t ct_total = 0U;
  std::unique_lock<std::mutex> k{_m_completions};
  std::queue<completion_t> deferred_completions;
  while ( ! _completions.empty() )
  {
    auto c = _completions.front();
    _completions.pop();
    k.unlock();
    auto &cq_entry{std::get<0>(c)};
    std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry.op_context));
    if ( cb_(g_context->context(), std::get<1>(c), cq_entry.flags, cq_entry.len, nullptr, cb_param_) == cb_acceptance::ACCEPT )
    {
      ++ct_total;
    }
    else
    {
      deferred_completions.push(c);
      ++_stats.defer_total;
      g_context.release();
    }
    k.lock();
  }
  std::swap(deferred_completions, _completions);
  return ct_total;
}

std::size_t Fabric_comm_grouped::drain_old_completions(const Component::IFabric_op_completer::complete_definite &cb_)
{
  std::size_t ct_total = 0U;
  std::unique_lock<std::mutex> k{_m_completions};
  while ( ! _completions.empty() )
  {
    auto c = _completions.front();
    _completions.pop();
    k.unlock();
    auto &cq_entry{std::get<0>(c)};
    std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry.op_context));
    cb_(g_context->context(), std::get<1>(c), cq_entry.flags, cq_entry.len, nullptr);
    ++ct_total;
    k.lock();
  }
  return ct_total;
}

std::size_t Fabric_comm_grouped::drain_old_completions(const Component::IFabric_op_completer::complete_tentative &cb_)
{
  std::size_t ct_total = 0U;
  std::unique_lock<std::mutex> k{_m_completions};
  std::queue<completion_t> deferred_completions;
  while ( ! _completions.empty() )
  {
    auto c = _completions.front();
    _completions.pop();
    k.unlock();
    auto &cq_entry{std::get<0>(c)};
    std::unique_ptr<async_req_record> g_context(static_cast<async_req_record *>(cq_entry.op_context));
    if ( cb_(g_context->context(), std::get<1>(c), cq_entry.flags, cq_entry.len, nullptr) == cb_acceptance::ACCEPT )
    {
      ++ct_total;
    }
    else
    {
      deferred_completions.push(c);
      ++_stats.defer_total;
      g_context.release();
    }
    k.lock();
  }
  std::swap(deferred_completions, _completions);
  return ct_total;
}

std::size_t Fabric_comm_grouped::poll_completions(const Component::IFabric_op_completer::complete_old &cb_)
{
  auto ct_total = drain_old_completions(cb_);

  bool drained = false;
  while ( ! drained )
  {
    std::size_t constexpr ct_max = 1;
    Fabric_op_control::fi_cq_entry_t entry;

    const auto ct = _conn.cq_read(&entry, ct_max);
    if ( ct < 0 )
    {
      switch ( const auto e = unsigned(-ct) )
      {
      case FI_EAVAIL:
        ct_total += process_cq_comp_err(cb_);
        break;
      case FI_EAGAIN:
        drained = true;
        break;
      case FI_EINTR:
        /* seen when profiling with gperftools */
        break;
      default:
        throw fabric_runtime_error(e, __FILE__, __LINE__);
      }
    }
    else
    {
      ct_total += process_or_queue_completion(entry, cb_, S_OK);
    }
  }

  _stats.ct_total += ct_total;
  return ct_total;
}

std::size_t Fabric_comm_grouped::poll_completions(const Component::IFabric_op_completer::complete_definite &cb_)
{
  auto ct_total = drain_old_completions(cb_);

  bool drained = false;
  while ( ! drained )
  {
    std::size_t constexpr ct_max = 1;
    Fabric_op_control::fi_cq_entry_t entry;

    const auto ct = _conn.cq_read(&entry, ct_max);
    if ( ct < 0 )
    {
      switch ( const auto e = unsigned(-ct) )
      {
      case FI_EAVAIL:
        ct_total += process_cq_comp_err(cb_);
        break;
      case FI_EAGAIN:
        drained = true;
        break;
      case FI_EINTR:
        /* seen when profiling with gperftools */
        break;
      default:
        throw fabric_runtime_error(e, __FILE__, __LINE__);
      }
    }
    else
    {
      ct_total += process_or_queue_completion(entry, cb_, S_OK);
    }
  }

  _stats.ct_total += ct_total;
  return ct_total;
}

std::size_t Fabric_comm_grouped::poll_completions_tentative(const Component::IFabric_op_completer::complete_tentative &cb_)
{
  std::size_t ct_total = 0U;
  bool drained = false;
  while ( ! drained )
  {
    std::size_t constexpr ct_max = 1;
    Fabric_op_control::fi_cq_entry_t entry;

    const auto ct = _conn.cq_read(&entry, ct_max);
    if ( ct < 0 )
    {
      switch ( const auto e = unsigned(-ct) )
      {
      case FI_EAVAIL:
        ct_total += process_cq_comp_err(cb_);
        break;
      case FI_EAGAIN:
        drained = true;
        break;
      case FI_EINTR:
        /* seen when profiling with gperftools */
        break;
      default:
        throw fabric_runtime_error(e, __FILE__, __LINE__);
      }
    }
    else
    {
      ct_total += process_or_queue_completion(entry, cb_, S_OK);
    }
  }

  ct_total += drain_old_completions(cb_);

  _stats.ct_total += ct_total;
  return ct_total;
}

std::size_t Fabric_comm_grouped::poll_completions(const Component::IFabric_op_completer::complete_param_definite &cb_, void *cb_param_)
{
  auto ct_total = drain_old_completions(cb_, cb_param_);

  bool drained = false;
  while ( ! drained )
  {
    std::size_t constexpr ct_max = 1;
    Fabric_op_control::fi_cq_entry_t entry;

    const auto ct = _conn.cq_read(&entry, ct_max);
    if ( ct < 0 )
    {
      switch ( const auto e = unsigned(-ct) )
      {
      case FI_EAVAIL:
        ct_total += process_cq_comp_err(cb_, cb_param_);
        break;
      case FI_EAGAIN:
        drained = true;
        break;
      case FI_EINTR:
        /* seen when profiling with gperftools */
        break;
      default:
        throw fabric_runtime_error(e, __FILE__, __LINE__);
      }
    }
    else
    {
      ct_total += process_or_queue_completion(entry, cb_, S_OK, cb_param_);
    }
  }

  _stats.ct_total += ct_total;
  return ct_total;
}

std::size_t Fabric_comm_grouped::poll_completions_tentative(const Component::IFabric_op_completer::complete_param_tentative &cb_, void *cb_param_)
{
  std::size_t ct_total = 0U;
  bool drained = false;
  while ( ! drained )
  {
    std::size_t constexpr ct_max = 1;
    Fabric_op_control::fi_cq_entry_t entry;

    const auto ct = _conn.cq_read(&entry, ct_max);
    if ( ct < 0 )
    {
      switch ( const auto e = unsigned(-ct) )
      {
      case FI_EAVAIL:
        ct_total += process_cq_comp_err(cb_, cb_param_);
        break;
      case FI_EAGAIN:
        drained = true;
        break;
      case FI_EINTR:
        /* seen when profiling with gperftools */
        break;
      default:
        throw fabric_runtime_error(e, __FILE__, __LINE__);
      }
    }
    else
    {
      ct_total += process_or_queue_completion(entry, cb_, S_OK, cb_param_);
    }
  }

  ct_total += drain_old_completions(cb_, cb_param_);

  _stats.ct_total += ct_total;
  return ct_total;
}

std::size_t Fabric_comm_grouped::stalled_completion_count()
{
  std::lock_guard<std::mutex> k{_m_completions};
  return _completions.size();
}

/**
 * Block and wait for next completion.
 *
 * @param polls_limit Maximum number of polls (throws exception on exceeding limit)
 *
 * @return Next completion context
 */
void Fabric_comm_grouped::wait_for_next_completion(std::chrono::milliseconds timeout)
{
  return _conn.wait_for_next_completion(timeout);
}

void Fabric_comm_grouped::wait_for_next_completion(unsigned polls_limit)
{
  return _conn.wait_for_next_completion(polls_limit);
}

/**
 * Unblock any threads waiting on completions
 *
 */
void Fabric_comm_grouped::unblock_completions()
{
  return _conn.unblock_completions();
}
