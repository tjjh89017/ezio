#include <memory>
#include <iostream>
#include <thread>
#include <chrono>
#include <getopt.h>
#include <stddef.h>

#include <libtorrent/libtorrent.hpp>

#include "daemon.hpp"
#include "service.hpp"
#include "thread_pool.hpp"

std::string server_address = "0.0.0.0:50051";

int main(int argc, char **argv)
{
  auto inst = ezio::thread_pool::get_instance();
  inst->start(4);
  ezio::io_job job1(server_address.c_str());
  inst->submit(std::move(job1));

  auto &daemon = ezio::ezio::get_instance();

  lt::settings_pack p;
  // setup alert mask
  p.set_int(lt::settings_pack::alert_mask,
            lt::alert_category::error | lt::alert_category::status);

  // disable all encrypt to avoid bug https://github.com/arvidn/libtorrent/issues/6735#issuecomment-1036675263
  p.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_disabled);
  p.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_disabled);

  lt::session_params ses_params(p);
  ses_params.disk_io_constructor = lt::default_disk_io_constructor;

  std::unique_ptr<lt::session> session =
    std::make_unique<lt::session>(ses_params);
  daemon.set_session(std::move(session));

  std::unique_ptr<ezio::gRPCService> grpcservice =
    std::make_unique<ezio::gRPCService>(server_address);

  daemon.set_grpcservice(std::move(grpcservice));

  std::cout << "Server listening on " << server_address << std::endl;
  daemon.wait(10);
  std::cout << "shutdown in main" << std::endl;

  return 0;
}
