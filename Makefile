PROG = ezio
STATIC_PROG = $(PROG)-static

CC =
CXX = g++
LD = g++
CXXFLAGS = -g -std=c++11 -I./ -pthread
LDFLAGS = -ltorrent-rasterbar -lboost_system -lstdc++ -lm -lgcc -lssl -lcrypto -lboost_program_options -lboost_chrono -lboost_random -ldl -lpthread
prefix = $(DESTDIR)/usr
sbindir = $(prefix)/sbin

PROTOC = protoc
GRPC_CPP_PLUGIN = grpc_cpp_plugin
GRPC_CPP_PLUGIN_PATH ?= `which $(GRPC_CPP_PLUGIN)`

PROTOS_PATH = ./
vpath %.proto $(PROTOS_PATH)

OBJS = main.o \
	config.o \
	raw_storage.o \
	logger.o

INC = 

PROTO_SRC = 
GRPC_OBJS = 
GRPC_INC = 
PYTHON_OBJS =

ifneq ($(GRPC),)
OBJS += service.o 
PROTO_SRC = ezio.proto
GRPC_OBJS += ezio.grpc.pb.o \
	ezio.pb.o
GRPC_INC += ezio.grpc.pb.h \
	ezio.pb.h
PYTHON_OBJS += ezio_pb2_grpc.py \
	ezio_pb2.py
CXXFLAGS += -DENABLE_GRPC
LDFLAGS += -lprotobuf -pthread -lpthread -lgrpc++ -lgrpc -lz
endif

.PHONY: all
all: $(PROG) client

.PHONY: static
static: $(STATIC_PROG)

$(PROG): $(OBJS) $(GRPC_OBJS) $(INC)
	$(LD) -o $(PROG) $(OBJS) $(GRPC_OBJS) $(LDFLAGS) $(CXXFLAGS)

$(STATIC_PROG): $(OBJS) $(GRPC_OBJS) $(INC)
	$(LD) -static -o $(STATIC_PROG) $(OBJS) $(GRPC_OBJS) $(LDFLAGS) $(CXXFLAGS)
	strip -s $(STATIC_PROG)

%.grpc.pb.cc: %.proto
	$(PROTOC) -I $(PROTOS_PATH) --grpc_out=. --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH) $<

%.pb.cc: %.proto
	$(PROTOC) -I $(PROTOS_PATH) --cpp_out=. $<

$(PYTHON_OBJS): $(PROTO_SRC)
	python3 -m grpc_tools.protoc -I./ --python_out=. --grpc_python_out=. $(PROTO_SRC)

%.o: %.cpp headers
	$(CXX) -c $(CXXFLAGS) $(LDFLAGS) -o $*.o $*.cpp

.PHONY: headers
headers: $(GRPC_OBJS:.o=.cc)

.PHONY: client
client: $(PYTHON_OBJS)

.PHONY: clean
clean:
	rm -rf $(OBJS) $(GRPC_OBJS) $(GRPC_OBJS:.o=.cc) $(GRPC_INC) $(PYTHON_OBJS) $(PROG) $(STATIC_PROG)
	make -C utils clean

install:
	mkdir -p $(sbindir)
	install -m 755 ezio $(sbindir)/
	install -m 755 ezio-static $(sbindir)/

.PHONY: netboot
netboot: static
	make -C utils all
