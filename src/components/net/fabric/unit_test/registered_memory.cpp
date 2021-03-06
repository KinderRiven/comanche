#include "registered_memory.h"

registered_memory::registered_memory(Component::IFabric_connection &cnxn_, std::size_t size_, std::uint64_t remote_key_)
  : _memory(size_)
  , _registration(cnxn_, &*_memory.begin(), _memory.size(), remote_key_, 0U)
{}
