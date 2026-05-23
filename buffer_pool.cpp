#include <spdlog/spdlog.h>

#include "buffer_pool.hpp"

namespace ezio
{

void watermark_callback(std::vector<std::weak_ptr<libtorrent::disk_observer>> const &cbs)
{
	for (auto const &i : cbs) {
		std::shared_ptr<libtorrent::disk_observer> o = i.lock();
		if (o) {
			o->on_disk();
		}
	}
}

buffer_pool::buffer_pool(libtorrent::io_context &ioc, size_t pool_size_bytes) :
	m_ios(ioc),
	m_size(0),
	m_max_use(pool_size_bytes / DEFAULT_BLOCK_SIZE),
	m_low_watermark((pool_size_bytes / DEFAULT_BLOCK_SIZE) / 2),
	m_high_watermark((pool_size_bytes / DEFAULT_BLOCK_SIZE) / 8 * 7),
	m_exceeded_max_size(false)
{
}

buffer_pool::~buffer_pool()
{
}

// All buffer_pool member functions run on the network thread (see contract
// in buffer_pool.hpp). No locking is required for internal state.

char *buffer_pool::allocate_buffer_impl()
{
	// Soft limit: m_max_use is a backpressure signal, not a hard cap.
	// We always try to allocate; only real malloc failure returns nullptr.
	// This matches libtorrent's disk_buffer_pool::allocate_buffer_impl().
	char *buf = (char *)malloc(DEFAULT_BLOCK_SIZE);
	if (!buf) {
		spdlog::debug("buffer pool malloc failed");
		m_exceeded_max_size = true;
		return nullptr;
	}
	m_size++;

	// Trigger exceeded flag at the high watermark (soft limit signal for backpressure).
	if (m_size >= m_high_watermark && !m_exceeded_max_size) {
		spdlog::debug("buffer pool reached high watermark, mem usage: {}", m_size);
		m_exceeded_max_size = true;
	}

	return buf;
}

char *buffer_pool::allocate_buffer()
{
	return allocate_buffer_impl();
}

char *buffer_pool::allocate_buffer(bool &exceeded, std::shared_ptr<libtorrent::disk_observer> o)
{
	char *buf = allocate_buffer_impl();

	if (m_exceeded_max_size) {
		exceeded = true;
		if (o) {
			m_observers.push_back(o);
		}
	}

	return buf;
}

void buffer_pool::free_disk_buffer(char *buf)
{
	free(buf);
	m_size--;
	check_buffer_level();
}

void buffer_pool::check_buffer_level()
{
	if (!m_exceeded_max_size || m_size > m_low_watermark) {
		// still high usage
		return;
	}

	// lower than low watermark, reopen
	spdlog::debug("buffer pool lower than low watermark, reopen");
	m_exceeded_max_size = false;

	// Move observers out before posting so the callback owns them.
	// Safe to do without a lock: this runs on the network thread and so does
	// every push_back into m_observers.
	std::vector<std::weak_ptr<libtorrent::disk_observer>> cbs;
	m_observers.swap(cbs);
	post(m_ios, std::bind(&watermark_callback, std::move(cbs)));
}

}  // namespace ezio
