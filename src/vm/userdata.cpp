#include <vm/userdata.hpp>
#include <vm/gc.hpp>

namespace li {
	void gc::destroy(vm* L, userdata* o) { o->gc_destroy(L); }
	void gc::traverse(stage_context s, userdata* o) { o->trait_traverse(s); }
};