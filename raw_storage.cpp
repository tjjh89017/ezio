#include "raw_storage.hpp"
#include <spdlog/spdlog.h>

lt::storage_interface *
raw_storage::raw_storage_constructor(lt::storage_params const &params,
                                     lt::file_pool &)
{
  return new raw_storage(params.files, params.path);
}

raw_storage::raw_storage(lt::file_storage const &fs, const std::string tp)
  : m_files(fs), target_partition(tp), lt::storage_interface(fs)
{
  this->fd = open(target_partition.c_str(), O_RDWR);
  if(this->fd < 0) {
    SPDLOG_ERROR("failed to open({}) = {}", target_partition, this->fd);
    // TODO exit
  }
  return;
}

raw_storage::~raw_storage()
{
  close(this->fd);
}

void raw_storage::initialize(lt::storage_error &se)
{}

// assume no resume
bool raw_storage::has_any_file(lt::storage_error &ec)
{
  return false;
}

int raw_storage::readv(lt::span<lt::iovec_t const> bufs,
                       lt::piece_index_t piece, int offset,
                       lt::open_mode_t flags, lt::storage_error &ec)
{
  int index = 0;
  int i = 0;
  int ret = 0;
  unsigned long long device_offset = 0;
  unsigned long long fd_offset = 0; // A fd' point we read data from fd from
  unsigned long long cur_offset =
    0; // A pieces' point we have to write data until
  unsigned long long remain_len = 0;
  unsigned long long piece_sum = 0;
  unsigned long long data_len = 0;
  char *data_buf, *data_ptr = NULL;
  char filename[33]; // Should be the max length of file name

  // Get file name from offset
  index = m_files.file_index_at_offset(
    piece * std::uint64_t(m_files.piece_length()) + offset);
  memcpy(filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
  filename[m_files.file_name_len(index)] = 0;
  sscanf(filename, "%llx", &device_offset);

  // Caculate total piece size of previous files
  for(i = 0; i < index; i++)
    piece_sum += m_files.file_size(i);

  // Caculate the length of all bufs
  for(i = 0; i < bufs.size(); i++) {
    data_len += bufs[i].size();
  }
  data_buf = (char *)malloc(data_len);

  // Read fd to data_buf
  cur_offset = piece * std::uint64_t(m_files.piece_length()) + offset;
  fd_offset = device_offset + offset +
              piece * std::uint64_t(m_files.piece_length()) - piece_sum;
  remain_len =
    m_files.file_size(index) -
    (offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum);
  data_ptr = data_buf;
  while(data_len > 0) {
    if(data_len > remain_len) {
      ret += pread(this->fd, data_ptr, remain_len, fd_offset);
      data_len -= remain_len;
      data_ptr += remain_len;
      cur_offset += remain_len;
      index = m_files.file_index_at_offset(cur_offset);
      memcpy(filename, m_files.file_name_ptr(index),
             m_files.file_name_len(index));
      filename[m_files.file_name_len(index)] = 0;
      sscanf(filename, "%llx", &fd_offset);
      remain_len = m_files.file_size(index);
    }
    else {
      ret += pread(this->fd, data_ptr, data_len, fd_offset);
      data_len -= data_len;
    }
  }

  // Copy data_buf to bufs
  data_ptr = data_buf;
  for(i = 0; i < bufs.size(); i++) {
    memcpy(bufs[i].data(), data_ptr, bufs[i].size());
    data_ptr += bufs[i].size();
  }

  free(data_buf);
  return ret;
}

int raw_storage::writev(lt::span<lt::iovec_t const> bufs,
                        lt::piece_index_t piece, int offset,
                        lt::open_mode_t flags, lt::storage_error &ec)
{
  int index = 0;
  int i = 0;
  int ret = 0;
  unsigned long long device_offset = 0;
  unsigned long long fd_offset = 0; // A fd' point we write data to fd from
  unsigned long long cur_offset =
    0; // A pieces' point we have to read data until
  unsigned long long remain_len = 0;
  unsigned long long piece_sum = 0;
  unsigned long long data_len = 0;
  char *data_buf = NULL, *data_ptr = NULL;
  char filename[33]; // Should be the max length of file name

  // Get file name from offset
  index = m_files.file_index_at_offset(
    piece * std::uint64_t(m_files.piece_length()) + offset);
  memcpy(filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
  filename[m_files.file_name_len(index)] = 0;
  sscanf(filename, "%llx", &device_offset);

  // Caculate total piece size of previous files
  for(i = 0; i < index; i++)
    piece_sum += m_files.file_size(i);

  // Caculate the length of all bufs
  for(i = 0; i < bufs.size(); i++) {
    data_len += bufs[i].size();
  }

  // Merge all bufs into data_buf
  data_buf = (char *)malloc(data_len);
  data_ptr = data_buf;
  for(i = 0; i < bufs.size(); i++) {
    memcpy(data_ptr, bufs[i].data(), bufs[i].size());
    data_ptr += bufs[i].size();
  }

  // Write data_buf to fd
  cur_offset = piece * std::uint64_t(m_files.piece_length()) + offset;
  fd_offset = device_offset + offset +
              piece * std::uint64_t(m_files.piece_length()) - piece_sum;
  remain_len =
    m_files.file_size(index) -
    (offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum);
  data_ptr = data_buf;
  while(data_len > 0) {
    if(data_len > remain_len) {
      ret += pwrite(this->fd, data_ptr, remain_len, fd_offset);
      data_len -= remain_len;
      data_ptr += remain_len;
      cur_offset += remain_len;
      index = m_files.file_index_at_offset(cur_offset);
      memcpy(filename, m_files.file_name_ptr(index),
             m_files.file_name_len(index));
      filename[m_files.file_name_len(index)] = 0;
      sscanf(filename, "%llx", &fd_offset);
      remain_len = m_files.file_size(index);
    }
    else {
      ret += pwrite(this->fd, data_ptr, data_len, fd_offset);
      data_len -= data_len;
    }
  }
  free(data_buf);
  return ret;
}

// Not need
void raw_storage::rename_file(lt::file_index_t, std::string const &,
                              lt::storage_error &)
{
  assert(false);
}
lt::status_t raw_storage::move_storage(std::string const &, lt::move_flags_t,
                                       lt::storage_error &)
{
  return lt::status_t::no_error;
}
bool raw_storage::verify_resume_data(
  lt::add_torrent_params const &,
  lt::aux::vector<std::string, lt::file_index_t> const &, lt::storage_error &)
{
  return false;
}
void raw_storage::write_resume_data(lt::entry &rd, lt::storage_error &ec) const
{
  return;
}
void raw_storage::set_file_priority(
  lt::aux::vector<lt::download_priority_t, lt::file_index_t> &,
  lt::storage_error &)
{
  return;
};
void raw_storage::release_files(lt::storage_error &ec)
{
  return;
}
void raw_storage::delete_files(lt::remove_flags_t, lt::storage_error &)
{}

bool raw_storage::tick()
{
  return false;
};
