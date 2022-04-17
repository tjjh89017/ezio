#include "raw_disk_io.hpp"

using namespace libtorrent;

std::unique_ptr<libtorrent::disk_interface>
raw_disk_io_constructor(libtorrent::io_context &ioc,
                        libtorrent::settings_interface const &s,
                        libtorrent::counters &c)
{
  return std::make_unique<raw_disk_io>();
}

raw_disk_io::raw_disk_io()
{}

storage_holder raw_disk_io::new_torrent(storage_params const &p,
                                        std::shared_ptr<void> const &torrent)
{}

void raw_disk_io::remove_torrent(storage_index_t)
{}

void raw_disk_io::async_read(
  storage_index_t storage, peer_request const &r,
  std::function<void(disk_buffer_holder, storage_error const &)> handler,
  disk_job_flags_t flags)
{}

bool raw_disk_io::async_write(storage_index_t storage, peer_request const &r,
                              char const *buf, std::shared_ptr<disk_observer> o,
                              std::function<void(storage_error const &)> handler,
                              disk_job_flags_t flags)
{}

void raw_disk_io::async_hash(
  storage_index_t storage, piece_index_t piece, span<sha256_hash> v2,
  disk_job_flags_t flags,
  std::function<void(piece_index_t, sha1_hash const &, storage_error const &)>
    handler)
{}

void raw_disk_io::async_hash2(
  storage_index_t storage, piece_index_t piece, int offset,
  disk_job_flags_t flags,
  std::function<void(piece_index_t, sha256_hash const &, storage_error const &)>
    handler)
{}

void raw_disk_io::async_move_storage(
  storage_index_t storage, std::string p, move_flags_t flags,
  std::function<void(status_t, std::string const &, storage_error const &)>
    handler)
{}

void raw_disk_io::async_release_files(storage_index_t storage,
                                      std::function<void()> handler)
{}

void raw_disk_io::async_check_files(
  storage_index_t storage, add_torrent_params const *resume_data,
  aux::vector<std::string, file_index_t> links,
  std::function<void(status_t, storage_error const &)> handler)
{}

void raw_disk_io::async_stop_torrent(storage_index_t storage,
                                     std::function<void()> handler)
{}

void raw_disk_io::async_rename_file(
  storage_index_t storage, file_index_t index, std::string name,
  std::function<void(std::string const &, file_index_t, storage_error const &)>
    handler)
{}

void raw_disk_io::async_delete_files(
  storage_index_t storage, remove_flags_t options,
  std::function<void(storage_error const &)> handler)
{}

void raw_disk_io::async_set_file_priority(
  storage_index_t storage, aux::vector<download_priority_t, file_index_t> prio,
  std::function<void(storage_error const &,
                     aux::vector<download_priority_t, file_index_t>)>
    handler)
{}

void raw_disk_io::async_clear_piece(storage_index_t storage,
                                    piece_index_t index,
                                    std::function<void(piece_index_t)> handler)
{}

void raw_disk_io::update_stats_counters(counters &c) const
{}

std::vector<open_file_state> raw_disk_io::get_status(storage_index_t) const
{}

void raw_disk_io::abort(bool wait)
{}

void raw_disk_io::submit_jobs()
{}

void raw_disk_io::settings_updated()
{}

