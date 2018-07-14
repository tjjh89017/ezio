SHELL = bash

PROG = syscall.a

CXXFLAGS = -std=c++11
LDFLAGS =

SRC = $(wildcard *.cpp)
OBJS = $(patsubst %.cpp,%.o,$(SRC))


.cpp.o:
	$(CXX) $(CXXFLAGS) -c $< -o $@ 

.PHONY: all
all: $(PROG)

$(PROG): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

.PHONY: clean
clean:
	rm -rf $(PROG) $(OBJS)
