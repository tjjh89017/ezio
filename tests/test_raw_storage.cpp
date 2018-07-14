#include "gtest/gtest.h"
#include "raw_storage.hpp"
#include "memoryview.h"

class RawStorageTest : public ::testing::Test {
	protected:
		void SetUp() override {
			mem = MemoryView::get_instance();
		}

		void TearDown() override {
		}

		MemoryView *mem;
		lt::storage_error ec;
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
	char cbuf[] = "deadbeefDEADBEEF";
	lt::file::iovec_t bufs1 = {cbuf, 16};
	raw.writev(&bufs1, 1, 0, 0, 0, ec);

	MemoryView &memory = *mem;

	char *ptr = NULL;
	ptr = memory.get_memory(0);
	EXPECT_EQ(memcmp(ptr, "deadbeef", piece_size / 2), 0);
	ptr = memory.get_memory(piece_size / 2);
	EXPECT_NE(memcmp(ptr, "DEADBEEF", piece_size / 2), 0);
	ptr = memory.get_memory(0x100);
	EXPECT_EQ(memcmp(ptr, "DEADBEEF", piece_size / 2), 0);

	lt::file::iovec_t bufs2 = {cbuf, 8};
	raw.writev(&bufs2, 1, 1, 0, 0, ec);
	ptr = memory.get_memory(0x200);
	EXPECT_EQ(memcmp(ptr, "deadbeef", piece_size / 2), 0);
}
