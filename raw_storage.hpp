#ifndef __RAW_STORAGE_HPP__
#define __RAW_STORAGE_HPP__

#include <iostream>
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
  static lt::storage_interface *
  raw_storage_constructor(lt::storage_params const &params, lt::file_pool &);

  raw_storage(lt::file_storage const &fs, const std::string tp);
  ~raw_storage();

  void initialize(lt::storage_error &se);

  // assume no resume
  bool has_any_file(lt::storage_error &ec);
  int readv(lt::span<lt::iovec_t const> bufs, lt::piece_index_t piece,
            int offset, lt::open_mode_t flags, lt::storage_error &ec);
  int writev(lt::span<lt::iovec_t const> bufs, lt::piece_index_t piece,
             int offset, lt::open_mode_t flags, lt::storage_error &ec);

  // Not need
  void rename_file(lt::file_index_t, std::string const &, lt::storage_error &);
  lt::status_t move_storage(std::string const &, lt::move_flags_t,
                            lt::storage_error &);
  bool
  verify_resume_data(lt::add_torrent_params const &,
                     lt::aux::vector<std::string, lt::file_index_t> const &,
                     lt::storage_error &);
  void write_resume_data(lt::entry &rd, lt::storage_error &ec) const;
  void set_file_priority(
    lt::aux::vector<lt::download_priority_t, lt::file_index_t> &,
    lt::storage_error &);
  void release_files(lt::storage_error &ec);
  void delete_files(lt::remove_flags_t, lt::storage_error &);
  bool tick();

private:
  lt::file_storage m_files;
  int fd;
  const std::string target_partition;
};

#endif
