#include "buffer_pool.hpp"

namespace ezio {

buffer_pool::buffer_pool()
{
  m_buffer = new char[MAX_BUFFER_POOL_SIZE];

  // put into deque
  for(int i = 0; i < MAX_BUFFER_POOL_SIZE; i += DEFAULT_BLOCK_SIZE) {
    m_deque.push_back(m_buffer + i);
  }
}

buffer_pool::~buffer_pool()
{
  m_deque.clear();
  delete m_buffer;
}

char *buffer_pool::allocate_buffer()
{
  std::unique_lock<std::mutex> l(m_pool_mutex);

  // no memory
  if(m_deque.empty()) {
    return nullptr;
  }

  char *buf = m_deque.front();
  m_deque.pop_front();
  return buf;
}

void buffer_pool::free_disk_buffer(char *buf)
{
  std::unique_lock<std::mutex> l(m_pool_mutex);
  m_deque.push_back(buf);
}

} // namespace ezio
