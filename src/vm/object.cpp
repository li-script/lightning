#include <vm/object.hpp>
#include <vm/gc.hpp>

namespace li {
	// Internal type-set implementation.
	//
	struct type_set : gc::node<type_set> {
		static constexpr size_t min_size        = 512;

		vclass* entries[];

		// Container interface.
		//
		size_t   size() { return this->object_bytes() / sizeof(vclass*); }
		vclass** begin() { return &entries[0]; }
		vclass** end() { return &entries[size()]; }

		// Resizing.
		//
		[[nodiscard]] type_set* nextsize(vm* L) {
			size_t n = size();
			auto ts = L->alloc<type_set>((n + (n >> 1)) * sizeof(vclass*));
			auto   it = std::copy(begin(), end(), ts->begin());
			std::fill(it, ts->end(), nullptr);
			L->gc.free(L, this);
			return ts;
		}
	};

	void typeset_init(vm* L) {
		L->typeset = L->alloc<type_set>(type_set::min_size * sizeof(vclass*));
		range::fill(*L->typeset, nullptr);
	}
	void typeset_sweep(vm* L, gc::stage_context s) {
		// gc::destroy of vclass handles this?
		//
		//for (auto& k : *L->typeset) {
		//	if (k && k->is_free()) {
		//		k = nullptr;
		//	}
		//}
	}
	vclass* typeset_fetch(vm* L, type id) {
		if (id >= type::obj)
			return nullptr;
		int32_t idx = -(int32_t(id) + 1);
		if (L->typeset->size() <= idx)
			return nullptr;
		return L->typeset->entries[idx];
	}
	static void typeset_push(vm* L, vclass* cl) {
		int32_t idx;
		if (auto it = range::find(*L->typeset, nullptr); it != L->typeset->end()) {
			idx = int32_t(it - L->typeset->begin());
		} else {
			idx = int32_t(L->typeset->size());
			L->typeset = L->typeset->nextsize(L);
		}
		L->typeset->entries[idx] = cl;
		cl->vm_tid               = -(idx + 1);
	}

	// GC hooks and maintainance of typeset.
	//
	void gc::destroy(vm* L, object* o) {
		if (o->gc_hook)
			o->gc_hook(o);
	}
	void gc::destroy(vm* L, vclass* o) {
		if (o->vm_tid < 0) {
			auto& e = L->typeset->entries[-(o->vm_tid + 1)];
			LI_ASSERT(e == o);
			e = nullptr;
		}
	}

	// Traversal of object and class types.
	//
	void gc::traverse(stage_context s, object* o) {
		o->cl->gc_tick(s);
		for (auto& f : o->cl->fields()) {
			if (f.value.is_static) {
				continue;
			}

			bool unk = f.value.ty == type::any;
			if (!unk && !is_gc_data(f.value.ty))
				continue;
			auto* p = &o->data[f.value.offset];

			if (unk) {
				any v = *((const any_t*) p);
				if (v.is_gc())
					v.as_gc()->gc_tick(s);
			} else {
				auto v = *((gc::header* const*) p);
				v->gc_tick(s);
			}
		}
	}
	void gc::traverse(stage_context s, vclass* o) {
		if (o->super)
			o->super->gc_tick(s);
		if (o->name)
			o->name->gc_tick(s);
		o->ctor->gc_tick(s);
		for (auto& f : o->fields()) {
			f.key->gc_tick(s);

			bool unk = f.value.ty == type::any;
			if (!unk && !is_gc_data(f.value.ty))
				continue;

			const void* p;
			if (f.value.is_static) {
				p = &o->static_space()[f.value.offset];
			} else {
				p = &o->default_space()[f.value.offset];
			}

			if (unk) {
				any v = *((const any_t*) p);
				if (v.is_gc())
					v.as_gc()->gc_tick(s);
			} else {
				auto v = *((gc::header* const*) p);
				if (v)
					v->gc_tick(s);
			}
		}
	}

	// Duplicates the object.
	//
	object* object::duplicate(vm* L) {
		if (data != context) {
			return this;
		}

		object* result = L->duplicate(this);
		result->type_id = cl->vm_tid;
		return result;
	}

	// Instantiates an object type.
	//
	object* object::create(vm* L, vclass* cl) {
		object* result  = L->alloc<object>(cl->object_length);
		result->cl      = cl;
		result->type_id = cl->vm_tid;
		result->data    = result->context;
		memcpy(result->context, cl->default_space(), cl->object_length);

		//for (auto& f : cl->fields()) {
		//	if (f.value.is_static) {
		//		continue;
		//	}
		//
		//	bool unk = f.value.ty == type::any;
		//	if (!unk && !is_gc_data(f.value.ty))
		//		continue;
		//
		//	auto* p = &result->data[f.value.offset];
		//	if (unk) {
		//		*((any_t*) p) = ((any_t*) p)->duplicate(L);
		//	} else {
		//		*((gc::header**) p) = any(*((gc::header**) p)).duplicate(L).as_gc();
		//	}
		//}
		return result;
	}

	// Instantiates a new class type.
	//
	vclass* vclass::create(vm* L, string* name, std::span<const field_pair> fields) {
		msize_t obj_len = 0;
		msize_t cls_len = 0;
		for (auto& f : fields) {
			if (f.value.is_static) {
				cls_len = std::max<msize_t>(f.value.offset + 8, cls_len);
			} else {
				obj_len = std::max<msize_t>(f.value.offset + 8, obj_len);
			}
		}

		vclass* r = L->alloc<vclass>(fields.size_bytes() + obj_len + cls_len);
		memset(r->static_data, 0, obj_len + cls_len);
		r->cxx_tid       = util::type_id_v<void>;
		r->name          = name;
		r->object_length = obj_len;
		r->static_length = cls_len;
		r->num_fields    = (msize_t) fields.size();
		range::copy(fields, r->fields().begin());
		typeset_push(L, r);
		return r;
	}

	// Get/Set, setter returns false if it threw an error.
	//
	any_t object::get(string* k) const {
		for (auto& [kx, v] : cl->fields()) {
			if (k == kx) {
				const void* p;
				if (v.is_static) {
					p = &cl->static_space()[v.offset];
				} else {
					p = &data[v.offset];
				}
				return any::load_from(p, v.ty);
			}
		}
		return nil;
	}
	bool object::set(vm* L, string* k, any_t vv) {
		for (auto& [kx, v] : cl->fields()) {
			if (k == kx) {
				void* p;
				if (v.is_static) {
					if (!v.is_dyn) [[unlikely]] {
						L->error("modifying constant field.");
						return false;
					}
					p = &cl->static_space()[v.offset];
				} else {
					p = &data[v.offset];
				}
				any(vv).store_at(p, v.ty);
				return true;
			}
		}
		L->error("field does not exist.");
		return false;
	}
};