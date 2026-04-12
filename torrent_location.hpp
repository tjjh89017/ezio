#ifndef __TORRENT_LOCATION_HPP__
#define __TORRENT_LOCATION_HPP__

#include <tuple>

#include <boost/functional/hash.hpp>
#include <libtorrent/libtorrent.hpp>

namespace ezio
{
// refer libtorrent torrent_location
class torrent_location
{
public:
	libtorrent::storage_index_t torrent;
	libtorrent::piece_index_t piece;
	int offset;

	torrent_location(libtorrent::storage_index_t const t, libtorrent::piece_index_t const p, int o) :
		torrent(t), piece(p), offset(o)
	{
	}
	bool operator==(torrent_location const &rhs) const
	{
		return std::tie(torrent, piece, offset) == std::tie(rhs.torrent, rhs.piece, rhs.offset);
	}
};
}  // namespace ezio

namespace std
{
template<>
struct hash<ezio::torrent_location> {
	using argument_type = ezio::torrent_location;
	using result_type = std::size_t;
	std::size_t operator()(argument_type const &l) const
	{
		std::size_t ret = 0;
		boost::hash_combine(ret, std::hash<libtorrent::storage_index_t>{}(l.torrent));
		boost::hash_combine(ret, std::hash<libtorrent::piece_index_t>{}(l.piece));
		boost::hash_combine(ret, std::hash<int>{}(l.offset));
		return ret;
	}
};

}  // namespace std

#endif
