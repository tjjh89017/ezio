#include <thread>
#include <chrono>
#include "daemon.hpp"

namespace ezio {

ezio::ezio()
{}

ezio &ezio::get_instance()
{
  static ezio instance;
  return instance;
}

void ezio::stop()
{
  shutdown_ = true;
}

void ezio::wait(int sec)
{
  while(!shutdown_) {
    std::this_thread::sleep_for(std::chrono::seconds(sec));
  }

  grpcservice_->stop();
}

void ezio::set_session(std::unique_ptr<lt::session> s)
{
  session_.swap(s);
}

lt::session *ezio::get_session()
{
  return session_.get();
}

void ezio::set_grpcservice(std::unique_ptr<gRPCService> g)
{
  grpcservice_.swap(g);
}

gRPCService *ezio::get_grpcservice()
{
  return grpcservice_.get();
}

} // namespace ezio
