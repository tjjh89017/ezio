cmake_minimum_required(VERSION 3.16)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

project(ezio CXX)

set(CMAKE_CXX_STANDARD 20)

set(sources
	main
	config
	raw_storage
)

set(grpc_sources
	service
)

set(protos
	ezio.proto
)

set(EZIO ezio)

option(grpc_support "Build EZIO with gRPC support" OFF)

set(LibtorrentRasterbar_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_STATIC_RUNTIME OFF)

if(grpc_support)
	find_package(Protobuf 3 REQUIRED)
	find_package(GRPC REQUIRED)

	protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${protos})
	PROTOBUF_GENERATE_GRPC_CPP(GRPC_SRCS GRPC_HDRS ${protos})

	list(APPEND sources ${PROTO_SRCS})
	list(APPEND sources ${GRPC_SRCS})
	list(APPEND sources ${grpc_sources})
	set(GRPC_DEFINE "ENABLE_GRPC")
endif()

add_executable(${EZIO}
	${sources}
)

# depends
find_package(LibtorrentRasterbar 1.2 REQUIRED)
find_package(Boost 1.56 REQUIRED COMPONENTS system program_options chrono random)

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
	${Boost_LIBRARIES}

	${PROTOBUF_LIBRARIES}
	${GRPC_LIBRARIES}
)

install(TARGETS ${EZIO}
	RUNTIME DESTINATION sbin)
