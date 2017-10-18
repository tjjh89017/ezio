#ifndef __CORE_EXT_HPP__
#define __CORE_EXT_HPP__

// Bytes BEGIN
inline unsigned long long int operator "" _KiB(const unsigned long long int size) {
	return size * 1024;
}

inline unsigned long long int operator "" _MiB(const unsigned long long int size) {
	return size * 1024_KiB;
}

inline unsigned long long int operator "" _GiB(const unsigned long long int size) {
	return size * 1024_MiB;
}
// Bytes END

// Time BEGIN
inline unsigned long long int operator "" _min(const unsigned long long int mins) {
	return mins * 60;
}
inline unsigned long long int operator "" _mins(const unsigned long long int mins) {
	return mins * 1_min;
}

inline unsigned long long int operator "" _hr(const unsigned long long int hrs) {
	return hrs * 60_min;
}
inline unsigned long long int operator "" _hrs(const unsigned long long int hrs) {
	return hrs * 1_hr;
}

inline unsigned long long int operator "" _day(const unsigned long long int days) {
	return days * 24_hr;
}
inline unsigned long long int operator "" _days(const unsigned long long int days) {
	return days * 1_day;
}
// Time END

#endif // ifndef __CORE_EXT_HPP__
