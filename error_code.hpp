#ifndef __ERROR_CODE_HPP__
#define __ERROR_CODE_HPP__

#include <boost/system/error_code.hpp>

namespace ezio {
	namespace errors {
		enum error_code_enum {
			no_error = 0,
			failed_to_open_disk,

			error_code_max
		}; // enum error_code_enum

		boost::system::error_code make_error_code(error_code_enum e);
		boost::system::error_category& category();
	}; // namespace errors
}; // namespace ezio

namespace boost { namespace system {
	template<> struct is_error_code_enum<ezio::errors::error_code_enum>
	{ static const bool value = true; };
} }; // namespace boost::system

#endif // ifndef __ERROR_CODE_HPP__
