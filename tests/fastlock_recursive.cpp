#include <util/fastlock.hpp>

namespace fastlock_test {
	bool recursively_try_lock_from_another_translation_unit(li::util::fastlock& lock) {
		if (!lock.try_lock()) {
			return false;
		}
		lock.unlock();
		return true;
	}
};
