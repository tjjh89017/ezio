#include "gtest/gtest.h"
#include "raw_storage.hpp"
#include "writer.hpp"

class RawStorageTest : public ::testing::Test {
    protected:
        void SetUp() override {
        }

        void TearDown() override {
        }

        lt::storage_error ec;
};

class mock_writer : public raw_writer {
    public:
        mock_writer() : off(0) {}
        int write(int fd, const void *buf, size_t count, off_t offset)
        {
            EXPECT_EQ(expect_offset[off], offset) << "off: " << off;
            EXPECT_EQ(expect_content[off].size(), count) << "off: " << off;
            EXPECT_EQ(0, memcmp(expect_content[off].c_str(), (const char *)buf, count)) << "off: " << off;
            off++;
            return count;
        }

        void add_expect(std::string str, off_t n) 
        { 
            expect_content.push_back(str); 
            expect_offset.push_back(n);
        }

    private:
        std::vector<off_t> expect_offset;
        std::vector<std::string> expect_content;
        int off;
};


TEST_F(RawStorageTest, TestWrite1) {
    int const piece_size = 16;
    lt::file_storage fs;
    char const f1[] = "000";
    char const f2[] = "100";
    char const f3[] = "200";
    fs.set_piece_length(piece_size);
    fs.add_file_borrow(f1, 3, std::string("test"), piece_size / 2);
    fs.add_file_borrow(f2, 3, std::string("test"), piece_size / 2);
    fs.add_file_borrow(f3, 3, std::string("test"), piece_size / 2);
    fs.set_num_pieces(int((fs.total_size() + piece_size - 1) / piece_size));

    raw_storage raw(fs, "/dev/null");

    mock_writer writer;
    writer.add_expect("deadbeef", 0);
    writer.add_expect("DEADBEEF", 0x100);
    writer.add_expect("deadbeef", 0x200);
    raw.set_writer(&writer);

    char cbuf[] = "deadbeefDEADBEEF";
    lt::file::iovec_t bufs1 = {cbuf, 16};
    raw.writev(&bufs1, 1, 0, 0, 0, ec);

    lt::file::iovec_t bufs2 = {cbuf, 8};
    raw.writev(&bufs2, 1, 1, 0, 0, ec);
}
