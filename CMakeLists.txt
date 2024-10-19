cmake_minimum_required(VERSION 3.16)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

project(ezio CXX)

set(CMAKE_CXX_STANDARD 14)

set(sources
	main.cpp
	config.cpp
	daemon.cpp
	buffer_pool.cpp
	raw_disk_io.cpp
	service.cpp
	log.cpp
)

set(protos
	ezio.proto
)

set(EZIO ezio)

#option(grpc_support "Build EZIO with gRPC support" OFF)

set(LibtorrentRasterbar_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
#set(Boost_USE_STATIC_LIBS OFF)
#set(Boost_USE_STATIC_RUNTIME OFF)

# depends
find_package(LibtorrentRasterbar 2.0 REQUIRED CONFIG)
find_package(Boost 1.74 REQUIRED COMPONENTS system program_options)
#find_package(Protobuf 3 REQUIRED)
find_package(gRPC REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)

# gen proto
#protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${protos})
#get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
#protobuf_generate_cpp(GRPC_SRCS GRPC_HDRS ${protos} PLUGIN protoc-gen-grpc=${grpc_cpp_plugin_location})

#list(APPEND sources ${PROTO_SRCS})
#list(APPEND sources ${GRPC_SRCS})
#list(APPEND sources ${grpc_sources})

# generate git version
execute_process(COMMAND git describe --tags --dirty
	TIMEOUT 5
	OUTPUT_VARIABLE GIT_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

# if no git
if(GIT_VERSION STREQUAL "")
	set(GIT_VERSION 2.0.12)
endif()

# start compile part
add_executable(${EZIO}
	${sources}
	${protos}
)

get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
protobuf_generate(TARGET ${EZIO} LANGUAGE cpp)
protobuf_generate(TARGET ${EZIO} LANGUAGE grpc GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}")

target_compile_definitions(${EZIO} PUBLIC
	GIT_VERSION="${GIT_VERSION}"

	${GRPC_DEFINE}
	_LARGEFILE64_SOURCE
	_FILE_OFFSET_BITS=64

	TORRENT_USE_LIBCRYPTO
)

target_include_directories(${EZIO} PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(${EZIO} PRIVATE
	spdlog
	fmt
	Boost::boost
	Boost::system
	Boost::program_options
	LibtorrentRasterbar::torrent-rasterbar

	protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
)

install(TARGETS ${EZIO}
	RUNTIME DESTINATION sbin)
