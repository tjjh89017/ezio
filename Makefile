PROG = ezio
STATIC_PROG = static-$(PROG)

CC =
CXX = g++
LD = g++
CXXFLAGS = -std=c++11 -I./
LDFLAGS = -ltorrent-rasterbar -lboost_system -lpthread -lstdc++ -lm -lgcc -lssl -lcrypto -lboost_chrono -lboost_random -ldl

OBJS = main.o
INC = 

.PHONY: all
all: $(PROG)

.PHONY: static
static: $(STATIC_PROG)

$(PROG): $(OBJS) $(INC)
	$(LD) -o $(PROG) $(OBJS) $(LDFLAGS) $(CXXFLAGS)

$(STATIC_PROG): $(OBJS) $(INC)
	$(LD) -static -o $(STATIC_PROG) $(OBJS) $(LDFLAGS) $(CXXFLAGS)
	strip -s $(STATIC_PROG)

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $(LDFLAGS) -o $*.o $*.cpp

.PHONY: clean
clean:
	rm -rf $(OBJS) $(PROG) $(STATIC_PROG)
	make -C utils clean

.PHONY: netboot
netboot: static
	make -C utils all
