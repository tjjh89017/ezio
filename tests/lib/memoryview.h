#ifndef _MEMORY_VIEW_H_
#define _MEMORY_VIEW_H_

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

#include <vector>

class MemoryView {
	private:
		static MemoryView *instance;
		MemoryView();
		char *memory;
		size_t size;
		unsigned long long offset;

	public:
		static MemoryView* get_instance();
		void set_memory_size(size_t s);
		void set_offset(unsigned long long off);
		char* get_memory(unsigned long long address);

};

#endif
