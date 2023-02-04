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
	m_exceeded_max_size(false)
{
	m_buffer = new char[MAX_BUFFER_POOL_SIZE];

	// put into deque
	for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i += DEFAULT_BLOCK_SIZE) {
		m_deque.push_back(m_buffer + i);
	}
}

buffer_pool::~buffer_pool()
{
	m_deque.clear();
	delete m_buffer;
}

char *buffer_pool::allocate_buffer_impl(std::unique_lock<std::mutex> &l)
{
	// no memory
	if (m_deque.empty()) {
		m_exceeded_max_size = true;
		return nullptr;
	}

	// reach high watermak, but still has some buffer to use
	if (BUFFER_COUNT - m_deque.size() > HIGH_WATERMARK) {
		m_exceeded_max_size = true;
	}

	char *buf = m_deque.front();
	m_deque.pop_front();
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
	m_deque.push_back(buf);
	check_buffer_level(l);
}

void buffer_pool::check_buffer_level(std::unique_lock<std::mutex> &l)
{
	if (!m_exceeded_max_size || m_deque.size() > LOW_WATERMARK) {
		// still high usgae
		return;
	}

	// lower than LOW_WATERMARKi, reopen
	m_exceeded_max_size = false;

	std::vector<std::weak_ptr<libtorrent::disk_observer>> cbs;
	m_observers.swap(cbs);
	// we could unlock mutex to let others allocate buffer
	l.unlock();
	post(m_ios, std::bind(&watermark_callback, std::move(cbs)));
}

}  // namespace ezio
