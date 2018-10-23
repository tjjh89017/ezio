#include "memoryview.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <cstring>
#include <sys/types.h>

#define find_origin_func(x) O_##x = dlsym(RTLD_NEXT, #x)


EXTERNC void init() __attribute__((constructor));
EXTERNC void init() {}

EXTERNC ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
	MemoryView *mem = MemoryView::get_instance();
	memcpy(buf, mem->get_memory(offset), count);
	return count;
}

EXTERNC ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
	MemoryView *mem = MemoryView::get_instance();
	memcpy(mem->get_memory(offset), buf, count);
	return count;
}
