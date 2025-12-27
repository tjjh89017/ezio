cmake_minimum_required(VERSION 3.16)

# Set policy for Boost package finding (CMake 3.30+)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

project(ezio CXX)

set(CMAKE_CXX_STANDARD 14)

set(sources
	main.cpp
	config.cpp
	daemon.cpp
	buffer_pool.cpp
	unified_cache.cpp
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
find_package(Boost 1.74 REQUIRED CONFIG COMPONENTS system program_options)
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

# ============================================================================
# Version Detection
# ============================================================================
include(Version)

# ============================================================================
# Build Configuration
# ============================================================================

# start compile part
add_executable(${EZIO}
	${sources}
	${protos}
)

get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
protobuf_generate(TARGET ${EZIO} LANGUAGE cpp)
protobuf_generate(TARGET ${EZIO} LANGUAGE grpc GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}")

target_compile_definitions(${EZIO} PUBLIC
	EZIO_VERSION="${EZIO_VERSION}"

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
