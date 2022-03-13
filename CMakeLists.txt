cmake_minimum_required(VERSION 3.16)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

project(ezio CXX)

set(CMAKE_CXX_STANDARD 20)

set(sources
	main
	#config
	#raw_storage

	daemon
	buffer_pool
)

set(grpc_sources
	service
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
find_package(LibtorrentRasterbar 2.0 REQUIRED)
find_package(Boost 1.56 REQUIRED COMPONENTS system)
find_package(Protobuf 3 REQUIRED)
find_package(GRPC REQUIRED)

# gen proto
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${protos})
PROTOBUF_GENERATE_GRPC_CPP(GRPC_SRCS GRPC_HDRS ${protos})

list(APPEND sources ${PROTO_SRCS})
list(APPEND sources ${GRPC_SRCS})
list(APPEND sources ${grpc_sources})


# start compile part
add_executable(${EZIO}
	${sources}
)

target_compile_definitions(${EZIO} PUBLIC
	${GRPC_DEFINE}
	_LARGEFILE64_SOURCE
	_FILE_OFFSET_BITS=64
)

target_include_directories(${EZIO} PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}
	${Boost_INCLUDE_DIRS}
	${LibtorrentRasterbar_INCLUDE_DIRS}

	${PROTOBUF_INCLUDE_DIRS}
)

target_link_libraries(${EZIO} PRIVATE
	${LibtorrentRasterbar_LIBRARIES}
	torrent-rasterbar
	${Boost_LIBRARIES}

	${PROTOBUF_LIBRARIES}
	${GRPC_LIBRARIES}
)

install(TARGETS ${EZIO}
	RUNTIME DESTINATION sbin)
