#include <iostream>
#include <thread>
#include <chrono>

#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/storage.hpp>
#include <libtorrent/io.hpp>

namespace lt = libtorrent;

class temp_storage : lt::storage_interface {
        temp_storage(lt::file_storage const& fs) : m_files(fs) {}
        void initialize(lt::storage_error& se) { return ; }
        bool has_any_file() { return false; }
        int readv(char* buf, int piece, int offset, int size)
        {
                std::map<int, std::vector<char> >::const_iterator i = m_file_data.find(piece);
                if (i == m_file_data.end()) return 0;
                int available = i->second.size() - offset;
                if (available <= 0) return 0;
                if (available > size) available = size;
                memcpy(buf, &i->second[offset], available);
                return available;
        }
        int writev(const char* buf, int piece, int offset, int size)
        {
                std::vector<char>& data = m_file_data[piece];
                if (data.size() < offset + size) data.resize(offset + size);
                std::memcpy(&data[offset], buf, size);
                return size;
        }
        bool rename_file(int file, std::string const& new_name)
        { assert(false); return false; }
        bool move_storage(std::string const& save_path) { return false; }
        bool verify_resume_data(lt::bdecode_node const& rd
                , std::vector<std::string> const* links
                , lt::storage_error& error) { return false; }
        bool write_resume_data(lt::entry& rd) const { return false; }
        void set_file_priority(std::vector<boost::uint8_t> const& prio, lt::storage_error& ec) {return ;}
        boost::int64_t physical_offset(int piece, int offset)
        { return piece * m_files.piece_length() + offset; };
        lt::sha1_hash hash_for_slot(int piece, lt::partial_hash& ph, int piece_size)
        {
                int left = piece_size - ph.offset;
                assert(left >= 0);
                if (left > 0)
                {
                        std::vector<char>& data = m_file_data[piece];
                        // if there are padding files, those blocks will be considered
                        // completed even though they haven't been written to the storage.
                        // in this case, just extend the piece buffer to its full size
                        // and fill it with zeroes.
                        if (data.size() < piece_size) data.resize(piece_size, 0);
                        ph.h.update(&data[ph.offset], left);
                }
                return ph.h.final();
        }
        bool release_files() { return false; }
        bool delete_files() { return false; }


        std::map<int, std::vector<char> > m_file_data;
        lt::file_storage m_files;
};


lt::storage_interface* temp_storage_constructor(lt::storage_params const& params)
{
        return new temp_storage(*params.files);
}

int main(int argc, char const* argv[])
{
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <magnet-url>" << std::endl;
    return 1;
  }
  lt::session ses;

  lt::add_torrent_params atp;
  atp.url = argv[1];
  atp.save_path = "."; // save in current dir
  // atp.storage = temp_storage_constructor;
  atp.storage = lt::default_storage_constructor;

  lt::torrent_handle h = ses.add_torrent(atp);

  for (;;) {
    std::vector<lt::alert*> alerts;
    ses.pop_alerts(&alerts);

    for (lt::alert const* a : alerts) {
      std::cout << a->message() << std::endl;
      // if we receive the finished alert or an error, we're done
      if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
        goto done;
      }
      if (lt::alert_cast<lt::torrent_error_alert>(a)) {
        goto done;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  done:
  std::cout << "done, shutting down" << std::endl;
}
