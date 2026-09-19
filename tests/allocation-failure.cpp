#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>

#include <util/platform.hpp>
#include <vm/gc.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>

#if LI_WINDOWS
	#include <fcntl.h>
	#include <io.h>
	#include <process.h>
#else
	#include <sys/types.h>
	#include <sys/wait.h>
	#include <unistd.h>
#endif

namespace {
	struct allocator_state {
		std::size_t allocation_requests = 0;
		std::size_t frees               = 0;
		std::size_t close_calls         = 0;
		std::size_t fail_request        = std::numeric_limits<std::size_t>::max();
		bool        failure_returned    = false;
		bool        announce_failure    = false;
		int         marker_fd           = -1;
	};

	void emit_failure_marker(allocator_state& state) {
		if (state.marker_fd >= 0) {
			const char marker = 'F';
#if LI_WINDOWS
			_write(state.marker_fd, &marker, 1);
#else
			ssize_t written;
			do {
				written = ::write(state.marker_fd, &marker, 1);
			} while (written < 0 && errno == EINTR);
#endif
		}
		std::fprintf(stderr, "allocation-failure child: custom allocator rejected later page request %zu\n", state.allocation_requests);
		std::fflush(stderr);
	}

	void* controlled_allocator(void* context, void* pointer, std::size_t page_count, bool executable) {
		auto& state = *static_cast<allocator_state*>(context);
		if (pointer == context && page_count == 0) {
			++state.close_calls;
			return nullptr;
		}
		if (pointer) {
			++state.frees;
			return li::platform::page_alloc(nullptr, pointer, page_count, executable);
		}
		++state.allocation_requests;
		if (state.allocation_requests == state.fail_request) {
			state.failure_returned = true;
			if (state.announce_failure)
				emit_failure_marker(state);
			return nullptr;
		}
		return li::platform::page_alloc(nullptr, nullptr, page_count, executable);
	}

	[[noreturn]] void fail(const char* message) {
		std::fprintf(stderr, "allocation-failure: %s\n", message);
		std::exit(1);
	}

	void initial_allocation_failure() {
		allocator_state state;
		state.fail_request = 1;
		li::vm* machine    = li::vm::create(&controlled_allocator, &state);
		if (machine) {
			machine->close();
			fail("VM creation succeeded after its initial allocation returned null");
		}
		if (!state.failure_returned || state.allocation_requests != 1)
			fail("VM creation did not issue exactly one failed initial page request");
		if (state.frees != 0 || state.close_calls != 0)
			fail("failed initial VM creation tried to free or close an allocator state it never acquired");
		// Reaching here after vm::create returned nullptr is the observable guard
		// that no gc::page or vm placement construction ran on the null address.
	}

	struct oversized_probe : li::gc::leaf<oversized_probe> {};
	constexpr std::size_t later_probe_bytes = 8 * 1024 * 1024;

	int later_allocation_child(int marker_fd) {
		allocator_state state;
		state.marker_fd = marker_fd;
		li::vm* machine = li::vm::create(&controlled_allocator, &state);
		if (!machine) {
			std::fprintf(stderr, "allocation-failure child: VM creation failed before the later-allocation scenario\n");
			return 40;
		}

		state.fail_request     = state.allocation_requests + 1;
		state.announce_failure = true;
		std::fprintf(stderr,
			 "allocation-failure child: forcing bounded %zu-byte GC request; current runtime contract must terminate via panic on allocator failure\n",
			 later_probe_bytes);
		std::fflush(stderr);

		// vm::alloc deliberately turns a failed later page request into the runtime's
		// fatal "out of memory" panic. The parent process verifies SIGABRT (or the
		// platform abort status) and never treats it as a catchable script error.
		auto* probe = machine->alloc<oversized_probe>(later_probe_bytes);
		li::rc::release(machine, probe);
		machine->close();
		std::fprintf(stderr, "allocation-failure child: later allocation unexpectedly returned\n");
		return 41;
	}

#if LI_WINDOWS
	void later_allocation_failure(const char* executable) {
		int marker_pipe[2];
		if (_pipe(marker_pipe, 64, _O_BINARY) != 0)
			fail("could not create later-allocation diagnostic pipe");

		char marker_argument[32];
		std::snprintf(marker_argument, sizeof(marker_argument), "%d", marker_pipe[1]);
		const char* arguments[] = {executable, "--late-allocation-child", marker_argument, nullptr};
		intptr_t    status      = _spawnv(_P_WAIT, executable, arguments);
		_close(marker_pipe[1]);
		char marker = 0;
		int  count  = _read(marker_pipe[0], &marker, 1);
		_close(marker_pipe[0]);

		if (status == -1)
			fail("could not start later-allocation child process");
		if (count != 1 || marker != 'F')
			fail("later-allocation child aborted without reaching the configured allocator failure");
		// The UCRT reports abort() either as exit status 3 or, in release
		// configurations, as the STATUS_STACK_BUFFER_OVERRUN fast-fail code.
		constexpr intptr_t fast_fail_status = intptr_t(int32_t(0xC0000409));
		if (status != 3 && status != fast_fail_status)
			fail("later-allocation child did not terminate with the Windows abort status 3");
		std::fprintf(stderr, "allocation-failure: later page failure produced a fatal child abort status (not an ordinary VM error)\n");
	}
#else
	void later_allocation_failure() {
		int marker_pipe[2];
		if (::pipe(marker_pipe) != 0)
			fail("could not create later-allocation diagnostic pipe");

		pid_t child = ::fork();
		if (child < 0) {
			::close(marker_pipe[0]);
			::close(marker_pipe[1]);
			fail("could not fork later-allocation child process");
		}
		if (child == 0) {
			::close(marker_pipe[0]);
			int status = later_allocation_child(marker_pipe[1]);
			::close(marker_pipe[1]);
			::_exit(status);
		}

		::close(marker_pipe[1]);
		int   status = 0;
		pid_t waited;
		do {
			waited = ::waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);

		char    marker = 0;
		ssize_t count;
		do {
			count = ::read(marker_pipe[0], &marker, 1);
		} while (count < 0 && errno == EINTR);
		::close(marker_pipe[0]);

		if (waited != child)
			fail("could not wait for later-allocation child process");
		if (count != 1 || marker != 'F')
			fail("later-allocation child aborted without reaching the configured allocator failure");
		if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT)
			fail("later-allocation child did not terminate through the runtime panic SIGABRT contract");
		std::fprintf(stderr, "allocation-failure: later page failure produced fatal child SIGABRT (not an ordinary VM error)\n");
	}
#endif
}

int main(int argc, char** argv) {
	if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--late-allocation-child") {
		int marker_fd = argc == 3 ? std::atoi(argv[2]) : -1;
		return later_allocation_child(marker_fd);
	}
	if (argc != 1) {
		std::fprintf(stderr, "usage: %s\n", argv[0]);
		return 2;
	}

	initial_allocation_failure();
#if LI_WINDOWS
	later_allocation_failure(argv[0]);
#else
	later_allocation_failure();
#endif
	return 0;
}
