# ============================================================================
# Version Detection
# ============================================================================

# Function to extract version from version.txt (for git archive tarballs)
function(get_version_from_template OUTPUT_VAR)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt")
		file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" VERSION_TEMPLATE)
		string(STRIP "${VERSION_TEMPLATE}" VERSION_TEMPLATE)

		# Check if template has been substituted by git archive
		# %D format after substitution: "HEAD -> master, tag: v2.0.16, origin/master"
		# %D format before substitution: "$Format:%D$"

		if(VERSION_TEMPLATE MATCHES "\\$Format:")
			# Not substituted - not from git archive tarball
			set(${OUTPUT_VAR} "" PARENT_SCOPE)
		elseif(VERSION_TEMPLATE MATCHES "tag: v?([0-9]+\\.[0-9]+\\.[0-9]+[^,)]*)")
			# Found version tag
			set(${OUTPUT_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
		else()
			# Template substituted but no version tag found
			set(${OUTPUT_VAR} "" PARENT_SCOPE)
		endif()
	else()
		set(${OUTPUT_VAR} "" PARENT_SCOPE)
	endif()
endfunction()

# Try git describe first (for git repository)
execute_process(COMMAND git describe --tags --dirty
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	TIMEOUT 5
	OUTPUT_VARIABLE GIT_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)

# If not in git repo, try extracting from version.txt (for tarball)
if(GIT_VERSION STREQUAL "")
	get_version_from_template(TARBALL_VERSION)
endif()

# Determine final version
if(NOT GIT_VERSION STREQUAL "")
	# Git repository - use git describe
	set(EZIO_VERSION "${GIT_VERSION}")
	message(STATUS "Version from git: ${EZIO_VERSION}")
elseif(TARBALL_VERSION)
	# Source tarball - use extracted version from version.txt
	set(EZIO_VERSION "v${TARBALL_VERSION}")
	message(STATUS "Version from tarball: ${EZIO_VERSION}")
else()
	# Fallback - should rarely happen (only if not in git repo AND version.txt missing/not substituted)
	set(EZIO_VERSION "unknown")
	message(WARNING "Version detection failed, using fallback: ${EZIO_VERSION}")
endif()
