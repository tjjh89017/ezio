#ifndef __BUFFER_POOL_HPP__
#define __BUFFER_POOL_HPP__

#include <deque>
#include <vector>
#include <mutex>
#include <spdlog/spdlog.h>
#include <boost/core/noncopyable.hpp>
#include <libtorrent/libtorrent.hpp>

// 8 GB
#define MAX_BUFFER_POOL_SIZE (8ULL * 1024 * 1024 * 1024)
// 16 KB
#define DEFAULT_BLOCK_SIZE (16 * 1024)

// watermark for write buffer
#define MAX_BUFFER_COUNT (MAX_BUFFER_POOL_SIZE / DEFAULT_BLOCK_SIZE)
#define LOW_WATERMARK (MAX_BUFFER_POOL_SIZE / DEFAULT_BLOCK_SIZE / 8 * 1)
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
	char *allocate_buffer(bool& exceeded, std::shared_ptr<libtorrent::disk_observer> o);
	void free_disk_buffer(char *) override;
	void check_buffer_level(std::unique_lock<std::mutex> &l);

private:
	libtorrent::io_context &m_ios;
	std::mutex m_pool_mutex;
	bool m_exceeded_max_size;
	uint64_t m_counter;
	std::vector<std::weak_ptr<libtorrent::disk_observer>> m_observers;
};

}  // namespace ezio

#endif
