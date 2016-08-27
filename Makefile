PROG = ezio

CC =
CXX = g++
LD = g++
CXXFLAGS = -I./
LDFLAGS = -ltorrent-rasterbar

OBJS = main.o
INC = 

.PHONY: all
all: $(PROG)

$(PROG): $(OBJS) $(INC)
	$(LD) $(LDFLAGS) $(CXXFLAGS) -o $(PROG) $(OBJS)

.cpp.o:
	$(CXX) -c $(CXXFLAGS) -o $*.o $*.cpp

.PHONY: clean
clean:
	rm -rf $(OBJS) $(PROG)
