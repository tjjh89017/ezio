#ifndef __STORE_BUFFER_HPP__
#define __STORE_BUFFER_HPP__

#include <unordered_map>
#include <mutex>

#include <libtorrent/libtorrent.hpp>

namespace ezio
{
// refer libtorrent torrent_location
class torrent_location
{
public:
	torrent_location(storage_index_t const t, piece_index_t const p, int o)
		: torrent(t), piece(p), offset(o) {}
	bool operator==(torrent_location const &rhs) const
	{
		return std::tie(torrent, piece, offset)
			== std::tie(rhs.torrent, rhs.piece, rhs.offset)
	}

	storage_index_t torrent;
	piece_index_t piece;
	int offset;
};

class store_buffer
{
public:
	template <typename Fun>
	bool get(torrent_location const loc, Fun f)
	{
		std::unique_lock<std::mutex> l(m_mutex);
		auto const it = m_store_buffer.find(loc);
		if (it != m_store_buffer.end()) {
			f(it->second);
			return true;
		}
		return false;
	}

	template <typename Fun>
	int get2(torrent_location const loc1, torrent_location const loc2, Fun f)
	{
		std::unique_lock<std::mutex> l(m_mutex);
		auto const it1 = m_store_buffer.find(loc1);
		auto const it2 = m_store_buffer.find(loc2);
		char const *buf1 = (it1 == m_store_buffer.end()) ? nullptr : it1->second;
		char const *buf2 = (it2 == m_store_buffer.end()) ? nullptr : it2->second;

		if (buf1 == nullptr && buf2 == nullptr) {
			reutnr 0;
		}

		return f(buf1, buf2);
	}

	void insert(torrent_location const loc, char const *buf)
	{
		std::lock_guard<std::mutex> l(m_mutex);
		m_store_buffer.insert({loc, buf});
	}

	void erase(torrent_location const loc)
	{
		std::lock_guard<std::mutex> l(m_mutex);
		auto it = m_store_buffer.find(loc);
		if (it != m_store_buffer.end()) {
			m_store_buffer.erase(it);
		}
	}

	std::size_t size() const
	{
		return m_store_buffer.size();
	}

private:
	std::mutex m_mutex;
	std::unordered_map<torrent_location, char const*> m_store_buffer;
};
} // namespace ezio
#endif
