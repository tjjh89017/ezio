cmake_minimum_required(VERSION 3.0)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

project(ezio CXX)

set(CMAKE_CXX_STANDARD 11)

set(sources
	main
	config
	raw_storage
	logger
)

set(EZIO ezio)

option(static_runtime "Build EZIO with static runtime" OFF)
option(strip "Strip all symbol from static executable" OFF)

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

	if(strip)
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--strip-all")
	endif()
endif()
	

add_executable(${EZIO}
	${sources}
)

# depends
find_package(LibtorrentRasterbar 1.1 REQUIRED)
find_package(Boost 1.56 REQUIRED COMPONENTS system program_options chrono random)

target_include_directories(${EZIO} PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${Boost_INCLUDE_DIRS}
	${LibtorrentRasterbar_INCLUDE_DIRS}
)

target_link_libraries(${EZIO} PRIVATE
	${LibtorrentRasterbar_LIBRARIES}
	${Boost_LIBRARIES}
	-ldl
)

install(TARGETS ${EZIO}
	RUNTIME DESTINATION sbin)
