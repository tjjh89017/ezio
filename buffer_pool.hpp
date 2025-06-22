#ifndef __BUFFER_POOL_HPP__
#define __BUFFER_POOL_HPP__

#include <deque>
#include <vector>
#include <mutex>
#include <boost/asio.hpp>
#include <boost/core/noncopyable.hpp>
#include <libtorrent/libtorrent.hpp>

// 128 MB
#define MAX_BUFFER_POOL_SIZE (128ULL * 1024 * 1024)
// 16 KB
#define DEFAULT_BLOCK_SIZE (16 * 1024)

// watermark for write buffer
#define BUFFER_COUNT (MAX_BUFFER_POOL_SIZE / DEFAULT_BLOCK_SIZE)
// 50%
#define LOW_WATERMARK (MAX_BUFFER_POOL_SIZE / DEFAULT_BLOCK_SIZE / 2)
// 87.5%
#define HIGH_WATERMARK (MAX_BUFFER_POOL_SIZE / DEFAULT_BLOCK_SIZE / 8 * 7)

namespace ezio
{
class buffer_pool : public libtorrent::buffer_allocator_interface, boost::noncopyable
{
public:
	buffer_pool(libtorrent::io_context &ioc);
	~buffer_pool();

	char *allocate_buffer_impl(std::unique_lock<std::mutex> &l);
	char *allocate_buffer();
	char *allocate_buffer(bool &exceeded, std::shared_ptr<libtorrent::disk_observer> o);
	void free_disk_buffer(char *) override;
	void check_buffer_level(std::unique_lock<std::mutex> &l);

	void push_disk_buffer_holders(std::function f<void()>);
	void pop_disk_buffer_holders(int size);

private:
	libtorrent::io_context &m_ios;
	std::mutex m_pool_mutex;
	int m_size;
	bool m_exceeded_max_size;
	std::vector<std::weak_ptr<libtorrent::disk_observer>> m_observers;

	std::mutex m_disk_buffer_holders_mutex;
	std::deque<std::function<void()>> m_disk_buffer_holders;

	boost::asio::thread_pool m_erase_thread_pool;
};

}  // namespace ezio

#endif
