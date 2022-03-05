#ifndef __RAW_DISK_IO_HPP__
#define __RAW_DISK_IO_HPP__

#include <memory>
#include <libtorrent/libtorrent.hpp>

namespace ezio {

std::unique_ptr<lt::disk_interface> raw_disk_io_constructor(lt::io_context& ioc, lt::settings_interface const&, lt::counters&);

class raw_disk_io {

};

} // namespace ezio

#endif
