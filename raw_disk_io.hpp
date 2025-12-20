#ifndef __RAW_DISK_IO_HPP__
#define __RAW_DISK_IO_HPP__

#include <map>
#include <memory>
#include <deque>
#include <libtorrent/libtorrent.hpp>
#include <boost/asio.hpp>
#include "buffer_pool.hpp"
#include "unified_cache.hpp"

namespace ezio
{
std::unique_ptr<libtorrent::disk_interface>
raw_disk_io_constructor(libtorrent::io_context &ioc,
	libtorrent::settings_interface const &,
	libtorrent::counters &);

class partition_storage;

class raw_disk_io final : public libtorrent::disk_interface
{
private:
	buffer_pool m_buffer_pool;	// Unified pool (256MB, temporary I/O buffers)

	unified_cache m_cache;	// Persistent cache (512MB, delayed write + read cache)

	boost::asio::thread_pool read_thread_pool_;
	boost::asio::thread_pool write_thread_pool_;
	boost::asio::thread_pool hash_thread_pool_;

	// callbacks are posted on this
	libtorrent::io_context &ioc_;

	libtorrent::settings_interface const *m_settings;
	libtorrent::counters &m_stats_counters;

	std::map<libtorrent::storage_index_t, std::unique_ptr<partition_storage>> storages_;
	std::deque<libtorrent::storage_index_t> free_slots_;

public:
	raw_disk_io(libtorrent::io_context &ioc,
		libtorrent::settings_interface const &sett,
		libtorrent::counters &cnt);
	~raw_disk_io();

	// this is called when a new torrent is added. The shared_ptr can be
	// used to hold the internal torrent object alive as long as there are
	// outstanding disk operations on the storage.
	// The returned storage_holder is an owning reference to the underlying
	// storage that was just created. It is fundamentally a storage_index_t
	libtorrent::storage_holder new_torrent(libtorrent::storage_params const &p,
		std::shared_ptr<void> const &torrent) override;

	// remove the storage with the specified index. This is not expected to
	// delete any files from disk, just to clean up any resources associated
	// with the specified storage.
	void remove_torrent(libtorrent::storage_index_t) override;

	// perform a read or write operation from/to the specified storage
	// index and the specified request. When the operation completes, call
	// handler possibly with a disk_buffer_holder, holding the buffer with
	// the result. Flags may be set to affect the read operation. See
	// disk_job_flags_t.
	//
	// The disk_observer is a callback to indicate that
	// the store buffer/disk write queue is below the watermark to let peers
	// start writing buffers to disk again. When ``async_write()`` returns
	// ``true``, indicating the write queue is full, the peer will stop
	// further writes and wait for the passed-in ``disk_observer`` to be
	// notified before resuming.
	//
	// Note that for ``async_read``, the peer_request (``r``) is not
	// necessarily aligned to blocks (but it is most of the time). However,
	// all writes (passed to ``async_write``) are guaranteed to be block
	// aligned.
	void async_read(
		libtorrent::storage_index_t storage, libtorrent::peer_request const &r,
		std::function<void(libtorrent::disk_buffer_holder, libtorrent::storage_error const &)> handler,
		libtorrent::disk_job_flags_t flags = {}) override;

	bool async_write(libtorrent::storage_index_t storage, libtorrent::peer_request const &r,
		char const *buf, std::shared_ptr<libtorrent::disk_observer> o,
		std::function<void(libtorrent::storage_error const &)> handler,
		libtorrent::disk_job_flags_t flags = {}) override;

	// Compute hash(es) for the specified piece. Unless the v1_hash flag is
	// set (in ``flags``), the SHA-1 hash of the whole piece does not need
	// to be computed.
	//
	// The `v2` span is optional and can be empty, which means v2 hashes
	// should not be computed. If v2 is non-empty it must be at least large
	// enough to hold all v2 blocks in the piece, and this function will
	// fill in the span with the SHA-256 block hashes of the piece.
	void async_hash(
		libtorrent::storage_index_t storage, libtorrent::piece_index_t piece, libtorrent::span<libtorrent::sha256_hash> v2,
		libtorrent::disk_job_flags_t flags,
		std::function<void(libtorrent::piece_index_t, libtorrent::sha1_hash const &, libtorrent::storage_error const &)>
			handler) override;

	// computes the v2 hash (SHA-256) of a single block. The block at
	// ``offset`` in piece ``piece``.
	void async_hash2(libtorrent::storage_index_t storage, libtorrent::piece_index_t piece, int offset,
		libtorrent::disk_job_flags_t flags,
		std::function<void(libtorrent::piece_index_t, libtorrent::sha256_hash const &,
			libtorrent::storage_error const &)>
			handler) override;

	// called to request the files for the specified storage/torrent be
	// moved to a new location. It is the disk I/O object's responsibility
	// to synchronize this with any currently outstanding disk operations to
	// the storage. Whether files are replaced at the destination path or
	// not is controlled by ``flags`` (see move_flags_t).
	void async_move_storage(
		libtorrent::storage_index_t storage, std::string p, libtorrent::move_flags_t flags,
		std::function<void(libtorrent::status_t, std::string const &, libtorrent::storage_error const &)>
			handler) override;

	// This is called on disk I/O objects to request they close all open
	// files for the specified storage/torrent. If file handles are not
	// pooled/cached, it can be a no-op. For truly asynchronous disk I/O,
	// this should provide at least one point in time when all files are
	// closed. It is possible that later asynchronous operations will
	// re-open some of the files, by the time this completion handler is
	// called, that's fine.
	void async_release_files(
		libtorrent::storage_index_t storage,
		std::function<void()> handler = std::function<void()>()) override;

	// this is called when torrents are added to validate their resume data
	// against the files on disk. This function is expected to do a few things:
	//
	// if ``links`` is non-empty, it contains a string for each file in the
	// torrent. The string being a path to an existing identical file. The
	// default behavior is to create hard links of those files into the
	// storage of the new torrent (specified by ``storage``). An empty
	// string indicates that there is no known identical file. This is part
	// of the "mutable torrent" feature, where files can be reused from
	// other torrents.
	//
	// The ``resume_data`` points the resume data passed in by the client.
	//
	// If the ``resume_data->flags`` field has the seed_mode flag set, all
	// files/pieces are expected to be on disk already. This should be
	// verified. Not just the existence of the file, but also that it has
	// the correct size.
	//
	// Any file with a piece set in the ``resume_data->have_pieces`` bitmask
	// should exist on disk, this should be verified. Pad files and files
	// with zero priority may be skipped.
	void async_check_files(
		libtorrent::storage_index_t storage, libtorrent::add_torrent_params const *resume_data,
		libtorrent::aux::vector<std::string, libtorrent::file_index_t> links,
		std::function<void(libtorrent::status_t, libtorrent::storage_error const &)> handler) override;

	// This is called when a torrent is stopped. It gives the disk I/O
	// object an opportunity to flush any data to disk that's currently kept
	// cached. This function should at least do the same thing as
	// async_release_files().
	void async_stop_torrent(
		libtorrent::storage_index_t storage,
		std::function<void()> handler = std::function<void()>()) override;

	// This function is called when the name of a file in the specified
	// storage has been requested to be renamed. The disk I/O object is
	// responsible for renaming the file without racing with other
	// potentially outstanding operations against the file (such as read,
	// write, move, etc.).
	void async_rename_file(
		libtorrent::storage_index_t storage, libtorrent::file_index_t index, std::string name,
		std::function<void(std::string const &, libtorrent::file_index_t, libtorrent::storage_error const &)>
			handler) override;

	// This function is called when some file(s) on disk have been requested
	// to be removed by the client. ``storage`` indicates which torrent is
	// referred to. See session_handle for ``remove_flags_t`` flags
	// indicating which files are to be removed.
	// e.g. session_handle::delete_files - delete all files
	// session_handle::delete_partfile - only delete part file.
	void async_delete_files(
		libtorrent::storage_index_t storage, libtorrent::remove_flags_t options,
		std::function<void(libtorrent::storage_error const &)> handler) override;

	// This is called to set the priority of some or all files. Changing the
	// priority from or to 0 may involve moving data to and from the
	// partfile. The disk I/O object is responsible for correctly
	// synchronizing this work to not race with any potentially outstanding
	// asynchronous operations affecting these files.
	//
	// ``prio`` is a vector of the file priority for all files. If it's
	// shorter than the total number of files in the torrent, they are
	// assumed to be set to the default priority.
	void async_set_file_priority(
		libtorrent::storage_index_t storage,
		libtorrent::aux::vector<libtorrent::download_priority_t, libtorrent::file_index_t> prio,
		std::function<void(libtorrent::storage_error const &,
			libtorrent::aux::vector<libtorrent::download_priority_t, libtorrent::file_index_t>)>
			handler) override;

	// This is called when a piece fails the hash check, to ensure there are
	// no outstanding disk operations to the piece before blocks are
	// re-requested from peers to overwrite the existing blocks. The disk I/O
	// object does not need to perform any action other than synchronize
	// with all outstanding disk operations to the specified piece before
	// posting the result back.
	void async_clear_piece(libtorrent::storage_index_t storage, libtorrent::piece_index_t index,
		std::function<void(libtorrent::piece_index_t)> handler) override;

	// update_stats_counters() is called to give the disk storage an
	// opportunity to update gauges in the ``c`` stats counters, that aren't
	// updated continuously as operations are performed. This is called
	// before a snapshot of the counters are passed to the client.
	void update_stats_counters(libtorrent::counters &c) const override;

	// Return a list of all the files that are currently open for the
	// specified storage/torrent. This is is just used for the client to
	// query the currently open files, and which modes those files are open
	// in.
	std::vector<libtorrent::open_file_state> get_status(libtorrent::storage_index_t) const override;

	// this is called when the session is starting to shut down. The disk
	// I/O object is expected to flush any outstanding write jobs, cancel
	// hash jobs and initiate tearing down of any internal threads. If
	// ``wait`` is true, this should be asynchronous. i.e. this call should
	// not return until all threads have stopped and all jobs have either
	// been aborted or completed and the disk I/O object is ready to be
	// destructed.
	void abort(bool wait) override;

	// This will be called after a batch of disk jobs has been issues (via
	// the ``async_*`` ). It gives the disk I/O object an opportunity to
	// notify any potential condition variables to wake up the disk
	// thread(s). The ``async_*`` calls can of course also notify condition
	// variables, but doing it in this call allows for batching jobs, by
	// issuing the notification once for a collection of jobs.
	void submit_jobs() override;

	// This is called to notify the disk I/O object that the settings have
	// been updated. In the disk io constructor, a settings_interface
	// reference is passed in. Whenever these settings are updated, this
	// function is called to allow the disk I/O object to react to any
	// changed settings relevant to its operations.
	void settings_updated() override;

private:
};

}  // namespace ezio

#endif
