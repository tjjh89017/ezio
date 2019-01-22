find_library(CARES_LIBRARIES NAMES cares)
find_path(CARES_INCLUDE_DIR ares.h
	PATH_SUFFIXES include
)
find_package_handle_standard_args(cares DEFAULT_MSG CARES_LIBRARIES CARES_INCLUDE_DIR)
