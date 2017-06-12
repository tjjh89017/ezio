#include "error_code.hpp"

namespace ezio {
	namespace errors {
		struct ezio_error_category : boost::system::error_category {
			const char* name() const BOOST_SYSTEM_NOEXCEPT override { return "ezio"; }

			std::string message(int ev) const BOOST_SYSTEM_NOEXCEPT override
			{
				static char const* msgs[] = {
					"no error",
					"cannot open disk",
				};

				if (ev < 0 || ev >= int(sizeof(msgs) / sizeof(msgs[0])))
					return "Unknown error";

				return msgs[ev];
			}
			boost::system::error_condition default_error_condition(int ev) const BOOST_SYSTEM_NOEXCEPT override
			{ return boost::system::error_condition(ev, *this); }
		}; // struct ezio_error_category

		boost::system::error_code make_error_code(error_code_enum e)
		{
			return boost::system::error_code(e, category());
		}

		boost::system::error_category& category()
		{
			static ezio_error_category ezio_category;
			return ezio_category;
		}
	}; // namespace errors
}; // namespace ezio
