cmake_minimum_required(VERSION 3.18)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

project(ezio CXX)

set(CMAKE_CXX_STANDARD 20)

set(sources
	main
	config
	raw_storage
	logger
)

set(grpc_sources
	service
)

set(protos
	ezio.proto
)

set(EZIO ezio)

option(static_runtime "Build EZIO with static runtime" OFF)
option(strip "Strip all symbol from static executable" OFF)
option(grpc_support "Build EZIO with gRPC support" OFF)

set(LibtorrentRasterbar_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_STATIC_RUNTIME OFF)

if(static_runtime)
	set(EZIO "${EZIO}-static")
	set(LibtorrentRasterbar_USE_STATIC_LIBS ON)
	set(Boost_USE_STATIC_LIBS ON)
	set(Boost_USE_STATIC_RUNTIME ON)
	set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++ -static")
	set(CMAKE_LINK_SEARCH_START_STATIC 1)
	set(CMAKE_LINK_SEARCH_END_STATIC 1)

	set(static_runtime_LIBRARIES
		-ldl
	)

	if(strip)
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--strip-all")
	endif()
endif()

if(grpc_support)
	find_package(Protobuf 3 REQUIRED)
	find_package(GRPC REQUIRED)

	protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${protos})
	PROTOBUF_GENERATE_GRPC_CPP(GRPC_SRCS GRPC_HDRS ${protos})

	list(APPEND sources ${PROTO_SRCS})
	list(APPEND sources ${GRPC_SRCS})
	list(APPEND sources ${grpc_sources})
	set(GRPC_DEFINE "ENABLE_GRPC")

	if(static_runtime)
		find_package(CARES REQUIRED)
		list(APPEND static_runtime_LIBRARIES ${CARES_LIBRARIES})
		list(APPEND static_runtime_LIBRARIES "-lz -lpthread")
	endif()
endif()

add_executable(${EZIO}
	${sources}
)

# depends
find_package(LibtorrentRasterbar 1.1 REQUIRED)
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

	${static_runtime_LIBRARIES}
)

install(TARGETS ${EZIO}
	RUNTIME DESTINATION sbin)
