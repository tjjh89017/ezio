#ifndef __CACHE_HPP__
#define __CACHE_HPP__

#include <mutex>
#include <spdlog/spdlog.h>
#include "store_buffer.hpp"
#include "buffer_pool.hpp"

// default 16MB
#define DEFAULT_CACHE_SIZE (1ULL * 1024)

namespace ezio
{

template<class Key, class Value>
class lru_cache : public libtorrent::buffer_allocator_interface, boost::noncopyable
{
public:
	typedef Key key_type;
	typedef Value value_type;
	typedef std::list<key_type> list_type;
	typedef std::unordered_map<
		key_type,
		std::pair<value_type, typename list_type::iterator>>
		map_type;

	lru_cache(size_t capacity) :
		m_capacity(capacity)
	{
	}

	lru_cache() :
		m_capacity(DEFAULT_CACHE_SIZE)
	{
	}

	~lru_cache()
	{
		clear();
	}

	size_t size() const
	{
		return m_map.size();
	}

	size_t capacity() const
	{
		return m_capacity;
	}

	bool empty() const
	{
		return m_map.empty();
	}

	bool contains(const key_type &key)
	{
		return m_map.find(key) != m_map.end();
	}

	void insert_impl(const key_type &key, const value_type &value)
	{
		std::lock_guard<std::mutex> lock(m_cache_mutex);
		typename map_type::iterator i = m_map.find(key);
		if (i == m_map.end()) {
			// insert item into the cache, but first check if it is full
			if (size() >= m_capacity) {
				// cache is full, evict the least recently used item
				evict();
			}

			// insert the new item
			m_list.push_front(key);
			m_map[key] = std::make_pair(value, m_list.begin());
		}
	}

	boost::optional<value_type> get_impl(const key_type &key)
	{
		// lookup value in the cache
		typename map_type::iterator i = m_map.find(key);
		if (i == m_map.end()) {
			// value not in cache
			return boost::none;
		}

		// return the value, but first update its place in the most
		// recently used list
		typename list_type::iterator j = i->second.second;
		if (j != m_list.begin()) {
			// move item to the front of the most recently used list
			m_list.erase(j);
			m_list.push_front(key);

			// update iterator in map
			j = m_list.begin();
			const value_type &value = i->second.first;
			m_map[key] = std::make_pair(value, j);

			// return the value
			return value;
		} else {
			// the item is already at the front of the most recently
			// used list so just return it
			return i->second.first;
		}
	}

	void clear()
	{
		m_map.clear();
		m_list.clear();
	}

	void set_capacity(size_t max_capacity)
	{
		std::lock_guard<std::mutex> lock(m_cache_mutex);
		m_capacity = max_capacity;
		while (size() > m_capacity) {
			evict();
		}
	}

	char *allocate_buffer()
	{
		return (char *)malloc(DEFAULT_BLOCK_SIZE);
	}

	void free_disk_buffer(char *buf) override
	{
		free(buf);
	}

	void insert(torrent_location const loc, char const *buf1)
	{
		std::lock_guard<std::mutex> lock(m_cache_mutex);
		typename map_type::iterator i = m_map.find(loc);
		if (i == m_map.end()) {
			// insert item into the cache, but first check if it is full
			if (size() >= m_capacity) {
				// cache is full, evict the least recently used item
				evict();
			}

			char *buf = (char *)malloc(DEFAULT_BLOCK_SIZE);
			memcpy(buf, buf1, DEFAULT_BLOCK_SIZE);

			// insert the new item
			m_list.push_front(loc);
			m_map[loc] = std::make_pair(buf, m_list.begin());
		}
	}

	template<typename Fun>
	bool get(torrent_location const loc, Fun f)
	{
		std::lock_guard<std::mutex> lock(m_cache_mutex);

		auto it = m_map.find(loc);
		if (it != m_map.end()) {
			f(it->second.first);
			return true;
		}

		return false;
	}

	template<typename Fun>
	int get2(torrent_location const loc1, torrent_location const loc2, Fun f)
	{
		std::lock_guard<std::mutex> lock(m_cache_mutex);
		auto const it1 = m_map.find(loc1);
		auto const it2 = m_map.find(loc2);
		char const *buf1 = (it1 == m_map.end()) ? nullptr : it1->second.first;
		char const *buf2 = (it2 == m_map.end()) ? nullptr : it2->second.first;

		if (buf1 == nullptr && buf2 == nullptr) {
			return 0;
		}

		return f(buf1, buf2);
	}

private:
	void evict()
	{
		// evict item from the end of most recently used list
		typename list_type::iterator i = --m_list.end();
		auto it = m_map.find(*i);
		free(it->second.first);
		m_map.erase(*i);
		m_list.erase(i);
	}

private:
	map_type m_map;
	list_type m_list;
	size_t m_capacity;

	std::mutex m_cache_mutex;
};

}  // namespace ezio

#endif
