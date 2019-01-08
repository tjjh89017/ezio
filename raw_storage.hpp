#ifndef __RAW_STORAGE_HPP__
#define __RAW_STORAGE_HPP__

#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/storage.hpp>
#include <libtorrent/io.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/peer_info.hpp>

namespace lt = libtorrent;

class raw_storage : lt::storage_interface {

public:

	static lt::storage_interface* raw_storage_constructor(lt::storage_params const& params);

	raw_storage(lt::file_storage const& fs, const std::string tp);
	~raw_storage();

	void initialize(lt::storage_error& se);

	// assume no resume
	bool has_any_file(lt::storage_error& ec);
	int readv(lt::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, lt::storage_error& ec);
	int writev(lt::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, lt::storage_error& ec);

	// Not need
	void rename_file(int index, std::string const& new_filename, lt::storage_error& ec);

	int move_storage(std::string const& save_path, int flags, lt::storage_error& ec);
	bool verify_resume_data(lt::bdecode_node const& rd
					, std::vector<std::string> const* links
					, lt::storage_error& error);
	void write_resume_data(lt::entry& rd, lt::storage_error& ec) const;
	void set_file_priority(std::vector<boost::uint8_t> const& prio, lt::storage_error& ec);
	/* for libtorrent-rasterbar>=1.1.8 */
	void set_file_priority(std::vector<boost::uint8_t> & prio, lt::storage_error& ec);
	void release_files(lt::storage_error& ec);
	void delete_files(int i, lt::storage_error& ec);
	bool tick();

private:
	lt::file_storage m_files;
	int fd;
	const std::string target_partition;
};

#endif
