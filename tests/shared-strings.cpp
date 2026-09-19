#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace {
	using namespace li;

	[[noreturn]] void fail(const char* message) {
		std::fprintf(stderr, "shared string regression: %s\n", message);
		std::abort();
	}

	void require(bool condition, const char* message) {
		if (!condition)
			fail(message);
	}
}

int main() {
	using namespace li;

	vm* left  = vm::create();
	vm* right = vm::create();
	require(left && right, "failed to create VMs");

	string* left_key  = string::create(left, "cross-vm-content-key");
	string* right_key = string::create(right, "cross-vm-content-key");
	require(left_key != right_key, "private VM intern pools unexpectedly shared identity");
	require(any(left_key) == any(right_key), "equal private strings from different VMs compared unequal");
	require(any(left_key).hash() == any(right_key).hash(), "equal private strings produced different hashes");

	string* left_empty  = string::create(left);
	string* right_empty = string::create(right);
	require(left_empty != right_empty, "private empty strings unexpectedly shared identity");
	require(any(left_empty) == any(right_empty), "empty strings from different VMs compared unequal");
	require(any(left_empty).hash() == any(right_empty).hash(), "empty strings produced different hashes");

	string* left_binary      = string::create(left, std::string_view{"same\0left", 9});
	string* right_binary     = string::create(right, std::string_view{"same\0left", 9});
	string* different_binary = string::create(right, std::string_view{"same\0diff", 9});
	require(any(left_binary) == any(right_binary), "embedded-NUL string equality ignored content length");
	require(any(left_binary).hash() == any(right_binary).hash(), "embedded-NUL equal strings hashed differently");
	require(any(left_binary) != any(different_binary), "embedded-NUL string equality stopped at the first NUL");

	table* values = table::create(right);
	require(values->set(right, any(right_key), any(number(42))), "failed to populate lookup table");
	any_t found = values->get(right, any(left_key));
	require(found.is_num() && found.as_num() == 42, "cross-VM private string lookup missed equal content");

	string* shared_key           = shared::copy_string(left, left_key);
	string* shared_empty         = shared::copy_string(left, left_empty);
	string* bootstrap_key        = string::create(left, "in");
	string* shared_bootstrap_key = shared::copy_string(left, bootstrap_key);
	require(shared::is_shared(shared_key), "shared string copy remained private");
	require(shared::is_shared(shared_empty), "shared empty string copy remained private");
	require(shared::is_shared(shared_bootstrap_key), "shared registry reused a private allocator bootstrap string");
	require(any(shared_key) == any(right_key), "shared and private equal strings compared unequal");
	require(any(shared_key).hash() == any(right_key).hash(), "shared copy changed the content hash");
	require(any(shared_empty) == any(right_empty), "shared empty string compared unequal");

	rc::release(left, left_key);
	rc::release(left, left_empty);
	rc::release(left, left_binary);
	rc::release(left, bootstrap_key);
	left->close();

	require(shared_bootstrap_key->view() == "in", "shared bootstrap-name string depended on its source VM");
	require(any(shared_empty) == any(right_empty), "shared empty string depended on its closed source VM");
	require(values->contains(any(shared_key)), "shared string depended on its closed source VM");
	found = values->get(right, any(shared_key));
	require(found.is_num() && found.as_num() == 42, "shared string lookup failed after source VM shutdown");

	string* coerced_source = string::create(right, "owned-coercion");
	string* coerced        = any(coerced_source).coerce_str(right);
	rc::release(right, coerced_source);
	require(coerced->view() == "owned-coercion", "identity string coercion did not return ownership");
	rc::release(right, coerced);

	string* empty_concat = string::concat(right, shared_empty, shared_empty);
	require(empty_concat == shared_empty, "shared empty concatenation did not preserve interned identity");
	rc::release(right, empty_concat);

	string* reinterned = shared::copy_string(right, right_key);
	require(reinterned == shared_key, "shared string registry did not intern equal content globally");
	rc::release(right, reinterned);

	any positive_zero = number(0.0);
	any negative_zero = number(-0.0);
	require(positive_zero == negative_zero, "signed-zero equality changed");
	require(positive_zero.hash() == negative_zero.hash(), "signed-zero hash coherence changed");
	any not_a_number = number(std::numeric_limits<number>::quiet_NaN());
	require(!(not_a_number == not_a_number), "NaN unexpectedly became equal to itself");

	rc::release(right, shared_key);
	string* recreated = shared::copy_string(right, right_key);
	require(shared::is_shared(recreated) && any(recreated) == any(right_key), "shared registry retained a dangling entry after last release");
	rc::release(right, recreated);

	rc::release(right, shared_empty);
	rc::release(right, shared_bootstrap_key);
	rc::release(right, values);
	rc::release(right, right_key);
	rc::release(right, right_empty);
	rc::release(right, right_binary);
	rc::release(right, different_binary);
	right->close();
	return 0;
}
