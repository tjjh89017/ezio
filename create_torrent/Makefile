PROG = create_torrent

CC =
CXX = g++
LD = g++
CXXFLAGS = -std=c++11 -I./
LDFLAGS = -ltorrent-rasterbar -lboost_system -lpthread -lstdc++ -lm -lgcc -lssl -lcrypto -lboost_chrono -lboost_random -ldl

OBJS = create_torrent.o
INC = 

.PHONY: all
all: $(PROG)

$(PROG): $(OBJS) $(INC)
	$(LD) -o $(PROG) $(OBJS) $(LDFLAGS) $(CXXFLAGS)

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $(LDFLAGS) -o $*.o $*.cpp

.PHONY: clean
clean:
	rm -rf $(OBJS) $(PROG)
