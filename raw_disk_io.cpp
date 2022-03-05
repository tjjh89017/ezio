#include "raw_disk_io.hpp"

namespace ezio {

std::unique_ptr<lt::disk_interface> raw_disk_io_constructor(lt::io_context& ioc, lt::settings_interface const& s, lt::counters& c)
{
	return std::make_unique<raw_disk_io>(ioc); 
}

} // namespace ezio
