#ifndef __BUFFER_POOL_HPP__
#define __BUFFER_POOL_HPP__

#include <vector>
#include <boost/core/noncopyable.hpp>
#include <libtorrent/libtorrent.hpp>

// 256 MB (unified pool for read + write)
#define MAX_BUFFER_POOL_SIZE (256ULL * 1024 * 1024)
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
// Thread-safety contract:
// All allocate_buffer() / free_disk_buffer() calls MUST happen on the libtorrent
// network thread (the io_context this pool was constructed with).
//   - allocate_buffer() is invoked from async_read/async_write/async_hash before
//     the work is posted to a worker thread.
//   - free_disk_buffer() runs from disk_buffer_holder's destructor, which is
//     reached either when libtorrent invokes the handler (network thread per
//     disk_interface contract) or when our own handler lambda is posted back
//     to m_ioc.
// Because access is single-threaded, no mutex/atomic is needed for the
// internal state. in_use() is the one exception: it can be read from a stats
// thread, and may observe a torn or stale value, which is acceptable for
// reporting purposes.
class buffer_pool : public libtorrent::buffer_allocator_interface, boost::noncopyable
{
public:
	buffer_pool(libtorrent::io_context &ioc, size_t pool_size_bytes);
	~buffer_pool();

	char *allocate_buffer_impl();
	char *allocate_buffer();
	char *allocate_buffer(bool &exceeded, std::shared_ptr<libtorrent::disk_observer> o);
	void free_disk_buffer(char *) override;
	void check_buffer_level();

	// Get current number of buffers in use.
	// May be called from a stats thread; the read is racy but tolerated.
	int in_use() const
	{
		return m_size;
	}

private:
	libtorrent::io_context &m_ios;
	// All fields below are touched only on the network thread (see contract above).
	int m_size;
	int m_max_use;
	int m_low_watermark;
	int m_high_watermark;
	bool m_exceeded_max_size;
	std::vector<std::weak_ptr<libtorrent::disk_observer>> m_observers;
};

}  // namespace ezio

#endif
