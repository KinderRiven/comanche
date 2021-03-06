#ifndef _TEST_PINGPONG_CLIENT_H_
#define _TEST_PINGPONG_CLIENT_H_

#include "pingpong_stat.h"
#include <common/types.h> /* status_t */
#include <cstdint> /* int8_t, uint16_t, uint64_t */
#include <string>
#include <memory> /* shared_ptr */

namespace Component
{
  class IFabric;
  class IFabric_client;
}

class registered_memory;

class pingpong_client
{
  void check_complete(::status_t stat);

  std::shared_ptr<Component::IFabric_client> _cnxn;
  pingpong_stat _stat;
  std::uint8_t _id;

protected:
  void do_quit();
public:
  pingpong_client(
    Component::IFabric &fabric
    , const std::string &fabric_spec
    , const std::string ip_address
    , std::uint16_t port
    , std::uint64_t buffer_size
    , std::uint64_t remote_key_base
    , unsigned iteration_count
    , std::size_t msg_size
    , std::uint8_t id
  );
  pingpong_client(pingpong_client &&) = default;
  pingpong_client &operator=(pingpong_client &&) = default;

  ~pingpong_client();
  pingpong_stat time() const;
};

#endif
