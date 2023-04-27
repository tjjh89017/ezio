#include "buffer_pool.hpp"

namespace ezio
{

void watermark_callback(std::vector<std::weak_ptr<libtorrent::disk_observer>> const &cbs)
{
	for (auto const& i : cbs)
	{
		std::shared_ptr<libtorrent::disk_observer> o = i.lock();
		if (o) {
			o->on_disk();
		}
	}
}

buffer_pool::buffer_pool(libtorrent::io_context &ioc) :
	m_ios(ioc),
	m_exceeded_max_size(false),
	m_counter(0)
{

}

buffer_pool::~buffer_pool()
{

}

char *buffer_pool::allocate_buffer_impl(std::unique_lock<std::mutex> &l)
{
	spdlog::debug("allocate buffer impl");
	// log in MiB
	spdlog::debug("current m_counter: {} MiB", m_counter / 64);
	// no memory
	if (m_counter > MAX_BUFFER_COUNT) {
		m_exceeded_max_size = true;
		spdlog::debug("allocate buffer fail, max exceeded");
		return nullptr;
	}

	char *buf = (char*)malloc(DEFAULT_BLOCK_SIZE);
	// reach high watermak, but still has some buffer to use
	if (!buf || m_counter > HIGH_WATERMARK) {
		m_exceeded_max_size = true;
		spdlog::debug("allocate buffer, max exceeded");
	}

	m_counter += buf ? 1 : 0;
	// if buf is nullptr, it is ok
	return buf;
}

char *buffer_pool::allocate_buffer()
{
	std::unique_lock<std::mutex> l(m_pool_mutex);
	return allocate_buffer_impl(l);
}

char *buffer_pool::allocate_buffer(bool& exceeded, std::shared_ptr<libtorrent::disk_observer> o)
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
	spdlog::debug("release buffer");
	free(buf);
	m_counter--;
	check_buffer_level(l);
}

void buffer_pool::check_buffer_level(std::unique_lock<std::mutex> &l)
{
	if (!m_exceeded_max_size || m_counter > LOW_WATERMARK) {
		// still high usage
		spdlog::debug("still high usage, current m_counter: {} MiB", m_counter / 64);
		return;
	}

	spdlog::debug("low watermark hit");
	// lower than LOW_WATERMARK, reopen
	m_exceeded_max_size = false;

	std::vector<std::weak_ptr<libtorrent::disk_observer>> cbs;
	m_observers.swap(cbs);
	// we could unlock mutex to let others allocate buffer
	l.unlock();
	post(m_ios, std::bind(&watermark_callback, std::move(cbs)));
}

}  // namespace ezio
