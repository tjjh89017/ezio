#pragma once
#include <dlfcn.h>
#include <cstring>
#include <sys/types.h>

// interface that defines writer
class raw_writer {
public:
    virtual int write(int fd, const void *buf, size_t count, off_t offset) = 0;
};

class sys_writer : public raw_writer {
    int write(int fd, const void *buf, size_t count, off_t offset);
};
