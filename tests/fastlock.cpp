#include <util/fastlock.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fastlock_test {
	// Acquires recursively in a separately compiled caller to check owner identity across TUs.
	bool recursively_try_lock_from_another_translation_unit(li::util::fastlock& lock);

	template<typename Predicate>
	bool spin_until(Predicate predicate, std::chrono::steady_clock::duration timeout = std::chrono::seconds(5)) {
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!predicate()) {
			if (std::chrono::steady_clock::now() >= deadline) {
				return false;
			}
			std::this_thread::yield();
		}
		return true;
	}

	void require(bool condition, std::string_view message) {
		if (!condition) {
			throw std::runtime_error(std::string(message));
		}
	}

	void test_deep_recursion_and_handoff() {
		constexpr std::size_t recursion_depth = 70000;
		constexpr unsigned    abort_phase     = 99;

		li::util::fastlock lock;
		std::size_t        held_depth = 0;
		try {
			for (std::size_t depth = 0; depth != recursion_depth; ++depth) {
				if ((depth & 1) == 0) {
					lock.lock();
					++held_depth;
				} else if (lock.try_lock()) {
					++held_depth;
				} else {
					break;
				}
			}
		} catch (...) {
			while (held_depth != 0) {
				lock.unlock();
				--held_depth;
			}
			throw;
		}

		if (held_depth != recursion_depth) {
			while (held_depth != 0) {
				lock.unlock();
				--held_depth;
			}
			require(false, "try_lock failed before reaching a recursion depth above 65535");
		}

		struct Payload {
			std::uint64_t sequence = 0;
			std::uint64_t inverse  = 0;
		} payload;
		struct Result {
			bool timed_out                      = false;
			bool acquired_while_fully_held      = false;
			bool acquired_after_partial_release = false;
			bool acquired_after_final_release   = false;
			bool observed_publication           = false;
		} result;

		std::atomic<unsigned> phase = 0;
		std::thread           contender([&] {
			auto wait_for_phase = [&](unsigned expected) {
				const bool reached = spin_until([&] {
					const unsigned current = phase.load(std::memory_order_relaxed);
					return current == expected || current == abort_phase;
				});
				if (!reached || phase.load(std::memory_order_relaxed) == abort_phase) {
					result.timed_out = true;
					phase.store(abort_phase, std::memory_order_relaxed);
					return false;
				}
				return true;
			};

			if (!wait_for_phase(1)) {
				return;
			}
			result.acquired_while_fully_held = lock.try_lock();
			if (result.acquired_while_fully_held) {
				lock.unlock();
			}
			phase.store(2, std::memory_order_relaxed);

			if (!wait_for_phase(3)) {
				return;
			}
			result.acquired_after_partial_release = lock.try_lock();
			if (result.acquired_after_partial_release) {
				lock.unlock();
			}
			phase.store(4, std::memory_order_relaxed);

			if (!wait_for_phase(5)) {
				return;
			}
			result.acquired_after_final_release = spin_until([&] { return lock.try_lock(); });
			if (result.acquired_after_final_release) {
				result.observed_publication = payload.sequence == 0x123456789abcdef0ULL && payload.inverse == ~payload.sequence;
				lock.unlock();
			}
			phase.store(6, std::memory_order_relaxed);
		});

		auto wait_for_phase = [&](unsigned expected) {
			return spin_until([&] {
				const unsigned current = phase.load(std::memory_order_relaxed);
				return current == expected || current == abort_phase;
			}) && phase.load(std::memory_order_relaxed) == expected;
		};

		bool protocol_completed = true;
		phase.store(1, std::memory_order_relaxed);
		if (!wait_for_phase(2)) {
			protocol_completed = false;
		} else {
			for (std::size_t depth = 1; depth != recursion_depth; ++depth) {
				lock.unlock();
				--held_depth;
			}
			phase.store(3, std::memory_order_relaxed);
			if (!wait_for_phase(4)) {
				protocol_completed = false;
			} else {
				payload.sequence = 0x123456789abcdef0ULL;
				payload.inverse  = ~payload.sequence;
				lock.unlock();
				--held_depth;
				phase.store(5, std::memory_order_relaxed);
				protocol_completed = wait_for_phase(6);
			}
		}

		if (!protocol_completed) {
			phase.store(abort_phase, std::memory_order_relaxed);
		}
		while (held_depth != 0) {
			lock.unlock();
			--held_depth;
		}
		contender.join();

		require(protocol_completed && !result.timed_out, "deep-recursion handoff protocol timed out");
		require(!result.acquired_while_fully_held, "another thread acquired a recursively held lock");
		require(!result.acquired_after_partial_release, "partial recursive release exposed the lock");
		require(result.acquired_after_final_release, "another thread could not acquire after final release");
		require(result.observed_publication, "final unlock did not publish protected state to the next owner");
	}

	void test_cross_translation_unit_recursion() {
		li::util::fastlock lock;
		lock.lock();
		const bool recursively_acquired = recursively_try_lock_from_another_translation_unit(lock);
		lock.unlock();
		require(recursively_acquired, "the same thread was not recognized as owner across translation units");
	}

	void test_waiter_notification_with_barging() {
		constexpr std::size_t waiter_count         = 8;
		constexpr std::size_t waiter_rounds        = 32;
		constexpr std::size_t owner_reacquisitions = 256;

		struct State {
			std::uint64_t sequence = 0;
			std::uint64_t inverse  = ~std::uint64_t{0};
		} state;

		li::util::fastlock       lock;
		std::atomic<bool>        go               = false;
		std::atomic<bool>        invariant_failed = false;
		std::atomic<std::size_t> ready            = 0;
		std::atomic<std::size_t> attempted        = 0;
		std::vector<std::size_t> acquisitions(waiter_count, 0);
		std::vector<std::thread> waiters;
		waiters.reserve(waiter_count);

		lock.lock();
		for (std::size_t index = 0; index != waiter_count; ++index) {
			waiters.emplace_back([&, index] {
				ready.fetch_add(1, std::memory_order_relaxed);
				while (!go.load(std::memory_order_acquire)) {
					std::this_thread::yield();
				}
				for (std::size_t round = 0; round != waiter_rounds; ++round) {
					if (round == 0) {
						attempted.fetch_add(1, std::memory_order_relaxed);
					}
					lock.lock();
					if (state.inverse != ~state.sequence) {
						invariant_failed.store(true, std::memory_order_relaxed);
					}
					++state.sequence;
					state.inverse = ~state.sequence;
					++acquisitions[index];
					lock.unlock();
				}
			});
		}

		const bool all_ready = spin_until([&] { return ready.load(std::memory_order_relaxed) == waiter_count; });
		go.store(true, std::memory_order_release);
		const bool all_attempted = spin_until([&] { return attempted.load(std::memory_order_relaxed) == waiter_count; });
		if (all_attempted) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}

		for (std::size_t iteration = 0; iteration != owner_reacquisitions; ++iteration) {
			lock.unlock();
			lock.lock();
			if (state.inverse != ~state.sequence) {
				invariant_failed.store(true, std::memory_order_relaxed);
			}
			++state.sequence;
			state.inverse = ~state.sequence;
		}
		lock.unlock();

		for (auto& waiter : waiters) {
			waiter.join();
		}

		require(all_ready, "waiters did not become ready before the deadline");
		require(all_attempted, "waiters did not contend for the held lock before the deadline");
		require(!invariant_failed.load(std::memory_order_relaxed), "waiter or barging owner observed torn protected state");
		for (std::size_t count : acquisitions) {
			require(count == waiter_rounds, "a blocked waiter did not eventually complete every acquisition");
		}
		const std::uint64_t expected = waiter_count * waiter_rounds + owner_reacquisitions;
		require(state.sequence == expected && state.inverse == ~expected, "waiter notification lost protected work");
	}

	void test_mixed_lock_contention() {
		constexpr std::size_t   thread_count          = 8;
		constexpr std::size_t   operations_per_thread = 10000;
		constexpr std::uint64_t initial_value         = 0x31415926ULL;

		struct Ledger {
			std::uint64_t operations;
			std::uint64_t value;
			std::uint64_t inverse;
		} ledger{0, initial_value, ~initial_value};

		li::util::fastlock       lock;
		std::atomic<bool>        go               = false;
		std::atomic<bool>        invariant_failed = false;
		std::atomic<std::size_t> ready            = 0;
		std::vector<std::thread> workers;
		workers.reserve(thread_count);

		for (std::size_t index = 0; index != thread_count; ++index) {
			workers.emplace_back([&, index] {
				ready.fetch_add(1, std::memory_order_relaxed);
				while (!go.load(std::memory_order_acquire)) {
					std::this_thread::yield();
				}
				for (std::size_t iteration = 0; iteration != operations_per_thread; ++iteration) {
					if (((iteration + index) % 3) == 0) {
						while (!lock.try_lock()) {
							std::this_thread::yield();
						}
					} else {
						lock.lock();
					}

					if (ledger.value != initial_value + ledger.operations || ledger.inverse != ~ledger.value) {
						invariant_failed.store(true, std::memory_order_relaxed);
					}
					const std::uint64_t next_value = ledger.value + 1;
					ledger.value                   = next_value;
					if ((iteration & 255) == 0) {
						std::this_thread::yield();
					}
					ledger.inverse = ~next_value;
					++ledger.operations;
					lock.unlock();
				}
			});
		}

		const bool all_ready = spin_until([&] { return ready.load(std::memory_order_relaxed) == thread_count; });
		go.store(true, std::memory_order_release);
		for (auto& worker : workers) {
			worker.join();
		}

		const std::uint64_t expected_operations = thread_count * operations_per_thread;
		require(all_ready, "contention workers did not become ready before the deadline");
		require(!invariant_failed.load(std::memory_order_relaxed), "mixed lock/try_lock contention violated mutual exclusion");
		require(ledger.operations == expected_operations, "mixed contention lost completed operations");
		require(ledger.value == initial_value + expected_operations && ledger.inverse == ~ledger.value,
			 "mixed contention produced an incorrect final multi-field state");
	}
};

int main() {
	try {
		fastlock_test::test_deep_recursion_and_handoff();
		fastlock_test::test_cross_translation_unit_recursion();
		fastlock_test::test_waiter_notification_with_barging();
		fastlock_test::test_mixed_lock_contention();
	} catch (const std::exception& error) {
		std::cerr << "fastlock regression failure: " << error.what() << '\n';
		return 1;
	}
	return 0;
}
