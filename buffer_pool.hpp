#ifndef __BUFFER_POOL_HPP__
#define __BUFFER_POOL_HPP__

#include <deque>
#include <mutex>
#include <libtorrent/libtorrent.hpp>

// 256 MB
#define MAX_BUFFER_POOL_SIZE (256ULL * 1024 * 1024)
// 16 KB
#define DEFAULT_BLOCK_SIZE (16 * 1024)

namespace ezio {

class buffer_pool {
public:
  static buffer_pool *get_instance();

  char *allocate_buffer();
  void free_buffer(char *);

private:
  buffer_pool();
  ~buffer_pool();

  buffer_pool(const buffer_pool &) = delete;
  buffer_pool(buffer_pool &&) = delete;
  buffer_pool &operator=(const buffer_pool &) = delete;
  buffer_pool &operator=(buffer_pool &&) = delete;

  std::mutex m_pool_mutex;
  char *m_buffer;
  std::deque<char *> m_deque;
};

class buffer_recycler final : public libtorrent::buffer_allocator_interface {
public:
  void free_disk_buffer(char *buffer) override;
};

} // namespace ezio

#endif
