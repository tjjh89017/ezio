#ifndef __BUFFER_POOL_HPP__
#define __BUFFER_POOL_HPP__

#include <deque>
#include <mutex>

// 256 MB
#define MAX_BUFFER_POOL_SIZE (256ULL * 1024 * 1024)
// 16 KB
#define DEFAULT_BLOCK_SIZE (16 * 1024)

namespace ezio {

class buffer_pool {
public:
  buffer_pool();
  ~buffer_pool();

  char *allocate_buffer();
  void free_buffer(char *);

private:
  std::mutex m_pool_mutex;
  char *m_buffer;
  std::deque<char *> m_deque;
};

} // namespace ezio

#endif
