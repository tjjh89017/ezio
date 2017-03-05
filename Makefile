PROG = ezio
STATIC_PROG = $(PROG)-static

CC =
CXX = g++
LD = g++
CXXFLAGS = -g -std=c++11 -I./
LDFLAGS = -ltorrent-rasterbar -lboost_system -lpthread -lstdc++ -lm -lgcc -lssl -lcrypto -lboost_chrono -lboost_random -ldl
prefix = $(DESTDIR)/usr
sbindir = $(prefix)/sbin

OBJS = main.o
INC = 

.PHONY: all
all: $(PROG)

.PHONY: static
static: $(STATIC_PROG)

$(PROG): $(OBJS) $(INC)
	$(LD) -o $(PROG) $(OBJS) $(LDFLAGS) $(CXXFLAGS)

$(STATIC_PROG): $(OBJS) $(INC)
	$(LD) -static -pthread -o $(STATIC_PROG) $(OBJS) $(LDFLAGS) $(CXXFLAGS)
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

install:
	mkdir -p $(sbindir)
	install -m 755 ezio $(sbindir)/
	install -m 755 ezio-static $(sbindir)/
