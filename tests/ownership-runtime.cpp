#include <cstdio>
#include <cstdlib>
#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <string_view>
#include <util/user.hpp>
#include <vm/array.hpp>
#include <vm/function.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/traits.hpp>
#include <vm/weak.hpp>

namespace {
	using namespace li;

	[[noreturn]] void fail(const char* message) {
		std::fprintf(stderr, "ownership runtime regression: %s\n", message);
		std::abort();
	}

	void require(bool condition, const char* message) {
		if (!condition)
			fail(message);
	}

	void require_live(vm* L, uint64_t expected, const char* phase) {
		if (L->gc.live_objects == expected)
			return;
		std::fprintf(stderr, "ownership runtime regression: %s left %llu live objects, expected %llu\n", phase,
			 static_cast<unsigned long long>(L->gc.live_objects), static_cast<unsigned long long>(expected));
		std::abort();
	}

	void acyclic_graph(vm* L) {
		const uint64_t baseline = L->gc.live_objects;
		table*         root     = table::create(L);
		array*         children = array::create(L, 2);
		table*         leaf     = table::create(L);
		string*        root_key = string::create(L, "ownership_runtime_children_key");
		string*        leaf_key = string::create(L, "ownership_runtime_message_key");
		string*        message  = string::create(L, "ownership-runtime-alive");

		leaf->set(L, any(leaf_key), any(message));
		require(children->set(L, 0, any(leaf)), "failed to populate first graph edge");
		require(children->set(L, 1, any(leaf)), "failed to populate shared graph edge");
		root->set(L, any(root_key), any(children));

		rc::release(L, root_key);
		rc::release(L, leaf_key);
		rc::release(L, message);
		rc::release(L, leaf);
		rc::release(L, children);

		any_t stored_children = root->get(L, any(root_key));
		require(stored_children.is_arr(), "root lost its owned child array");
		any_t first_leaf  = stored_children.as_arr()->get(L, 0);
		any_t second_leaf = stored_children.as_arr()->get(L, 1);
		require(first_leaf.is_tbl() && first_leaf == second_leaf, "shared child identity was not retained");
		any_t stored_message = first_leaf.as_tbl()->get(L, any(leaf_key));
		require(stored_message.is_str() && stored_message.as_str()->view() == "ownership-runtime-alive",
			 "acyclic graph value did not survive borrowed local releases");

		rc::release(L, root);
		require_live(L, baseline, "acyclic graph destruction");
	}

	void deep_chain(vm* L) {
		const uint64_t     baseline = L->gc.live_objects;
		constexpr uint64_t depth    = 65536;
		array*             root     = array::create(L, 1);
		array*             current  = root;

		for (uint64_t index = 1; index < depth; ++index) {
			array* next = array::create(L, 1);
			require(current->set(L, 0, any(next)), "failed to link deep ownership chain");
			rc::release(L, next);
			current = next;
		}

		rc::release(L, root);
		require_live(L, baseline, "bounded deep chain destruction");
	}

	void replacement_self_alias(vm* L) {
		const uint64_t baseline = L->gc.live_objects;
		array*         holder   = array::create(L, 1);
		table*         target   = table::create(L);
		require(holder->set(L, 0, any(target)), "failed to install replacement target");
		rc::release(L, target);

		any_t borrowed = holder->get(L, 0);
		require(borrowed.is_tbl(), "replacement target was not retained by its slot");
		const rc::statistics before = rc::counts();
		require(holder->set(L, 0, borrowed), "self-alias replacement failed");
		const rc::statistics after = rc::counts();
		require(after.retains == before.retains && after.releases == before.releases, "identity replacement performed a destructive retain/release pair");
		require(holder->get(L, 0) == borrowed, "self-alias replacement changed identity");

		rc::release(L, holder);
		require_live(L, baseline, "self-alias replacement cleanup");
	}

	void error_unwinding(vm* L) {
		static constexpr std::string_view source   = R"li(
fn explode() {
   const payload = {child: [{value: 7}]}
   payload.child[nil]
}
explode()
)li";
		const uint64_t                    baseline = L->gc.live_objects;

		for (unsigned iteration = 0; iteration != 32; ++iteration) {
			any script = load_script(L, source, "ownership-runtime-error");
			require(!script.is_exc() && script.is_fn(), "failed to construct error-unwinding script");
			any result = L->call(0, script);
			require(result.is_exc(), "error-unwinding script unexpectedly succeeded");
			require(L->last_ex != nil, "runtime error did not retain its exception");
			rc::release(L, script);
			L->clear_exception();
			require_live(L, baseline, "runtime error unwinding");
		}
	}

	void copied_captures(vm* L) {
		static constexpr std::string_view source   = R"li(
fn exercise_copied_capture() {
   let payload = {answer: 42}
   const original = || { payload }
   const copied = original::dup()
   assert(copied != original)
   assert(copied() == payload)
   payload.answer = 43
   assert(original().answer == 43)
   assert(copied().answer == 43)
}
exercise_copied_capture()
)li";
		const uint64_t                    baseline = L->gc.live_objects;
		any                               script   = load_script(L, source, "ownership-runtime-captures");
		require(!script.is_exc() && script.is_fn(), "failed to construct copied-capture script");
		any result = L->call(0, script);
		require(!result.is_exc(), "copied-capture script failed");
		rc::release(L, result);
		rc::release(L, script);
		require_live(L, baseline, "copied capture cleanup");
	}

	void failed_script_construction(vm* L) {
		static constexpr std::string_view invalid_source = R"li(
fn broken( {
   const payload = [{value: 1}]
)li";
		const uint64_t                    baseline       = L->gc.live_objects;

		for (unsigned iteration = 0; iteration != 32; ++iteration) {
			any result = load_script(L, invalid_source, "ownership-runtime-invalid");
			require(result.is_exc(), "invalid script unexpectedly compiled");
			require(L->last_ex.is_str(), "parser failure did not retain its diagnostic");
			L->clear_exception();
			require_live(L, baseline, "failed script construction");
		}
	}

	any_t LI_CC native_reenter(vm* L, any* args, slot_t count) {
		if (count != 1 || !args[0].is_fn())
			return L->error("ownership reentry expected one function");

		string* module_key   = string::create(L, "ownership_runtime");
		any     module_value = L->modules->get(L, any(module_key));
		rc::release(L, module_key);
		if (!module_value.is_tbl())
			return L->error("ownership reentry module disappeared");

		string* callback_key = string::create(L, "reenter");
		bool    erased       = module_value.as_tbl()->erase(L, any(callback_key));
		rc::release(L, callback_key);
		if (!erased)
			return L->error("ownership reentry callback was not owned by its module");

		return L->call(0, args[0]);
	}

	void native_callback_reentry(vm* L) {
		static constexpr std::string_view source   = R"li(
import ownership_runtime
assert(ownership_runtime.reenter(|| {
   const payload = {answer: 42}
   payload.answer
}) == 42)
)li";
		const uint64_t                    baseline = L->gc.live_objects;
		function*                         callback = function::create(L, &native_reenter);
		util::export_as(L, "ownership_runtime.reenter", any(callback));
		rc::release(L, callback);

		any script = load_script(L, source, "ownership-runtime-reentry");
		require(!script.is_exc() && script.is_fn(), "failed to construct native reentry script");
		any result = L->call(0, script);
		require(!result.is_exc(), "native callback reentry failed");
		rc::release(L, result);
		rc::release(L, script);

		string* module_key = string::create(L, "ownership_runtime");
		bool    erased     = L->modules->erase(L, any(module_key));
		rc::release(L, module_key);
		require(erased, "failed to remove transient native callback module");
		require_live(L, baseline, "native callback reentry cleanup");
	}

	void weak_expiration(vm* L) {
		const uint64_t baseline = L->gc.live_objects;
		table*         target   = table::create(L);
		weak*          observer = weak::create(L, target);
		require_live(L, baseline + 3, "weak observer setup");
		require(!observer->expired(), "new weak observer was already expired");

		any_t locked = observer->lock();
		require(locked.is_tbl() && locked.as_tbl() == target, "weak lock did not preserve target identity");
		rc::release(L, target);
		require(!observer->expired(), "weak target expired while an owned lock remained");
		rc::release(L, locked);
		require(observer->expired(), "weak observer was not invalidated at target destruction");
		require(observer->lock() == nil, "expired weak observer returned a target");
		require_live(L, baseline + 1, "expired weak observer retention");

		rc::release(L, observer);
		require_live(L, baseline, "weak observer cleanup");
	}

	void explicit_strong_cycle(vm* L) {
		const uint64_t baseline = L->gc.live_objects;
		array*         first    = array::create(L, 1);
		array*         second   = array::create(L, 1);
		require(first->set(L, 0, any(second)), "failed to create first cycle edge");
		require(second->set(L, 0, any(first)), "failed to create second cycle edge");
		weak* observer = weak::create(L, first);
		rc::release(L, first);
		rc::release(L, second);

		require_live(L, baseline + 5, "intentionally retained strong cycle");
		require(!observer->expired(), "strong cycle was collected before being broken");
		any_t owned_first = observer->lock();
		require(owned_first.is_arr(), "failed to lock first cycle node");
		any_t owned_second = owned_first.as_arr()->get(L, 0);
		require(owned_second.is_arr(), "first cycle edge was lost");
		rc::retain(owned_second);
		require(owned_second.as_arr()->get(L, 0) == owned_first, "second cycle edge was lost");

		require(owned_first.as_arr()->set(L, 0, nil), "failed to break first cycle edge");
		require(owned_second.as_arr()->set(L, 0, nil), "failed to break second cycle edge");
		rc::release(L, owned_second);
		rc::release(L, owned_first);
		require(observer->expired(), "broken cycle still retained its target");
		rc::release(L, observer);
		require_live(L, baseline, "broken strong cycle cleanup");
	}

	unsigned  shutdown_finalizer_calls = 0;
	unsigned  shutdown_native_calls    = 0;
	unsigned  shutdown_user_calls      = 0;
	function* shutdown_user_callback   = nullptr;

	any_t LI_CC shutdown_nested_native(vm* L, any*, slot_t count) {
		if (count != 0)
			return L->error("shutdown nested callback expected no arguments");
		++shutdown_native_calls;
		return L->ok();
	}

	any_t LI_CC shutdown_finalizer(vm* L, any*, slot_t count) {
		if (count != 0)
			return L->error("shutdown finalizer expected no arguments");
		++shutdown_finalizer_calls;

		table*  temporary = table::create(L);
		string* key       = string::create(L, "shutdown-allocation-key");
		string* value     = string::create(L, "shutdown-allocation-value");
		temporary->set(L, any(key), any(value));
		any_t stored = temporary->get(L, any(key));
		if (!stored.is_str() || stored.as_str()->view() != "shutdown-allocation-value")
			return L->error("shutdown allocation did not survive publication");

		function* nested        = function::create(L, &shutdown_nested_native);
		any       nested_result = L->call(0, any(nested));
		rc::release(L, nested);
		if (nested_result.is_exc())
			return nested_result;
		rc::release(L, nested_result);

		function* user_callback = shutdown_user_callback;
		shutdown_user_callback  = nullptr;
		if (!user_callback)
			return L->error("shutdown user callback disappeared");
		any user_result = L->call(0, any(user_callback));
		rc::release(L, user_callback);
		if (user_result.is_exc())
			return user_result;
		if (!user_result.is_tbl()) {
			rc::release(L, user_result);
			return L->error("shutdown user callback returned a non-table");
		}
		string* user_key     = string::create(L, "message");
		any_t   user_message = user_result.as_tbl()->get(L, any(user_key));
		bool    user_ok      = user_message.is_str() && user_message.as_str()->view() == "shutdown-user-code";
		rc::release(L, user_key);
		rc::release(L, user_result);
		if (!user_ok)
			return L->error("shutdown user callback returned the wrong value");
		++shutdown_user_calls;

		rc::release(L, key);
		rc::release(L, value);
		rc::release(L, temporary);
		return L->ok();
	}

	void install_shutdown_finalizer(vm* L) {
		static constexpr std::string_view user_source = R"li(
|| {
   const temporary = {message: "shutdown-user-code"}
   temporary
}
)li";
		any                               script      = load_script(L, user_source, "ownership-runtime-shutdown");
		require(!script.is_exc() && script.is_fn(), "failed to construct shutdown user callback");
		any callback = L->call(0, script);
		rc::release(L, script);
		require(!callback.is_exc() && callback.is_fn(), "failed to create shutdown user callback");
		shutdown_user_callback = callback.as_fn();

		table*    target     = table::create(L);
		function* finalizer  = function::create(L, &shutdown_finalizer);
		any       set_result = set_trait(L, any(target), trait::del, any(finalizer));
		require(!set_result.is_exc(), "failed to install shutdown finalizer");
		rc::release(L, set_result);
		rc::release(L, finalizer);

		string* root_key = string::create(L, "ownership-runtime-shutdown-root");
		L->modules->set(L, any(root_key), any(target));
		rc::release(L, root_key);
		rc::release(L, target);
	}
}

int main() {
	li::vm* L = li::vm::create();
	if (!L)
		fail("failed to create VM");
	li::lib::register_std(L);
	li::rc::counts()        = {};
	const uint64_t baseline = L->gc.live_objects;

	acyclic_graph(L);
	deep_chain(L);
	replacement_self_alias(L);
	error_unwinding(L);
	copied_captures(L);
	failed_script_construction(L);
	native_callback_reentry(L);
	weak_expiration(L);
	explicit_strong_cycle(L);

	require_live(L, baseline, "complete ownership harness");
	const li::rc::statistics totals = li::rc::counts();
	require(totals.retains > 0, "retain counter did not observe ownership traffic");
	require(totals.releases > 0, "release counter did not observe ownership traffic");

	install_shutdown_finalizer(L);
	L->close();
	require(shutdown_finalizer_calls == 1, "shutdown finalizer did not run exactly once");
	require(shutdown_native_calls == 1, "shutdown finalizer did not re-enter native code");
	require(shutdown_user_calls == 1, "shutdown finalizer did not run synchronous user code");
	require(shutdown_user_callback == nullptr, "shutdown user callback ownership was not released");
	return 0;
}
