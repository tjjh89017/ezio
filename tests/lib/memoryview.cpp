#include "memoryview.h"

MemoryView* MemoryView::instance = 0;

MemoryView* MemoryView::get_instance() {
	if(instance == 0){
		instance = new MemoryView();
	}
	return instance;
}

MemoryView::MemoryView() : offset(0), size(10 * 1024 * 1024) {
	memory = new char[size];
}

void MemoryView::set_memory_size(size_t s) {
	size = s;
	delete [] memory;
	memory = new char[size];
}

void MemoryView::set_offset(unsigned long long off) {
	offset = off;
}

char* MemoryView::get_memory(unsigned long long address) {
	return memory + (address - offset);
}
