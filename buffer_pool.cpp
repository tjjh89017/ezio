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

buffer_pool::buffer_pool(libtorrent::io_context &ioc) :
	m_ios(ioc),
	m_size(0),
	m_max_use(BUFFER_COUNT),
	m_low_watermark(LOW_WATERMARK),
	m_high_watermark(HIGH_WATERMARK),
	m_exceeded_max_size(false)
{
}

buffer_pool::~buffer_pool()
{
}

char *buffer_pool::allocate_buffer_impl(std::unique_lock<std::mutex> &l)
{
	// no memory
	if (m_size >= m_max_use) {
		m_exceeded_max_size = true;
		spdlog::warn("buffer pool reach max buffer count");
		return nullptr;
	}

	// reach high watermark, but still has some buffer to use
	if (m_size > m_high_watermark) {
		spdlog::warn("buffer pool reach high watermark, mem usage: {}", m_size);
		m_exceeded_max_size = true;
	}

	char *buf = (char*)malloc(DEFAULT_BLOCK_SIZE);
	if (!buf) {
		spdlog::warn("buffer pool malloc failed");
		m_exceeded_max_size = true;
		return nullptr;
	}
	m_size++;
	return buf;
}

char *buffer_pool::allocate_buffer()
{
	std::unique_lock<std::mutex> l(m_pool_mutex);
	return allocate_buffer_impl(l);
}

char *buffer_pool::allocate_buffer(bool &exceeded, std::shared_ptr<libtorrent::disk_observer> o)
{
	std::unique_lock<std::mutex> l(m_pool_mutex);
	char *buf = allocate_buffer_impl(l);

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
	std::unique_lock<std::mutex> l(m_pool_mutex);
	free(buf);
	m_size--;
	check_buffer_level(l);
}

void buffer_pool::check_buffer_level(std::unique_lock<std::mutex> &l)
{
	if (!m_exceeded_max_size || m_size > m_low_watermark) {
		// still high usgae
		return;
	}

	// lower than LOW_WATERMARK, reopen
	spdlog::info("buffer pool lower than low watermark, reopen");
	m_exceeded_max_size = false;

	std::vector<std::weak_ptr<libtorrent::disk_observer>> cbs;
	m_observers.swap(cbs);
	// we could unlock mutex to let others allocate buffer
	l.unlock();
	post(m_ios, std::bind(&watermark_callback, std::move(cbs)));
}

void buffer_pool::set_settings(libtorrent::settings_interface const &sett)
{
	std::unique_lock<std::mutex> l(m_pool_mutex);

	// Read cache_size from settings (in KiB)
	int cache_size_kb = sett.get_int(libtorrent::settings_pack::cache_size);
	if (cache_size_kb == 0) {
		// Use default if not set
		cache_size_kb = MAX_BUFFER_POOL_SIZE / 1024;
	}

	// Convert to buffer count
	int new_max_use = (cache_size_kb * 1024) / DEFAULT_BLOCK_SIZE;
	if (new_max_use < 16) {
		// Minimum 16 buffers (256 KiB)
		new_max_use = 16;
	}

	// Calculate watermarks
	m_max_use = new_max_use;
	m_low_watermark = new_max_use / 2;                    // 50%
	m_high_watermark = new_max_use - new_max_use / 8;    // 87.5%

	spdlog::info("buffer pool settings updated: max_use={}, low_watermark={}, high_watermark={}",
		m_max_use, m_low_watermark, m_high_watermark);

	// Check if current usage exceeds new limit
	if (m_size >= m_max_use) {
		m_exceeded_max_size = true;
	}
}

}  // namespace ezio
