#include "writer.hpp"

int sys_writer::write(int fd, const void *buf, size_t count, off_t offset)
{
    return pwrite(fd, buf, count, offset);
}
