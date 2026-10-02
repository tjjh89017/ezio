#ifndef __PIECE_ROUTE_HPP__
#define __PIECE_ROUTE_HPP__

#include <cstddef>
#include <cstdint>

#include <libtorrent/libtorrent.hpp>

namespace ezio
{

// Piece indexes are consecutive, so the sum visits the threads round-robin;
// the storage index only shifts the starting thread.
inline size_t piece_route_index(libtorrent::storage_index_t storage,
	libtorrent::piece_index_t piece, size_t count)
{
	size_t const s = static_cast<size_t>(static_cast<std::uint32_t>(storage));
	size_t const p = static_cast<size_t>(static_cast<std::int32_t>(piece));
	return (s + p) % count;
}

}  // namespace ezio

#endif	// __PIECE_ROUTE_HPP__
