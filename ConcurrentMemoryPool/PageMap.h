#pragma once
#include"Common.h"

// 当 BITS 较大（如 35 以上）时，两层树会导致根节点或叶节点过大，
// 使用三层可以更均匀地分割索引位，减少单级数组长度。
// ---------- 三层基数树（64 位系统，BITS = 51 左右）----------
//// Three-level radix tree
template <int BITS>
class TCMalloc_PageMap3 {
private:
	// How many bits should we consume at each interior level
	static const int INTERIOR_BITS = (BITS + 2) / 3; // Round-up
	static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;

	// How many bits should we consume at leaf level
	static const int LEAF_BITS = BITS - 2 * INTERIOR_BITS;
	static const int LEAF_LENGTH = 1 << LEAF_BITS;

	// Interior node
	struct Node {
		Node* ptrs[INTERIOR_LENGTH];
	};

	// Leaf node
	struct Leaf {
		void* values[LEAF_LENGTH];
	};

	Node* root_;                          // Root of radix tree
	void* (*allocator_)(size_t);          // Memory allocator

	Node* NewNode() {
		Node* result = reinterpret_cast<Node*>((*allocator_)(sizeof(Node)));
		if (result != NULL) {
			memset(result, 0, sizeof(*result));
		}
		return result;
	}

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap3(void* (*allocator)(size_t)) {
		allocator_ = allocator;
		root_ = NewNode();
	}

	void* get(Number k) const {
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);
		if ((k >> BITS) > 0 ||
			root_->ptrs[i1] == NULL || root_->ptrs[i1]->ptrs[i2] == NULL) {
			return NULL;
		}
		return reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3];
	}

	void set(Number k, void* v) {
		assert(k >> BITS == 0);
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);
		reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3] = v;
	}

	bool Ensure(Number start, size_t n) {
		for (Number key = start; key <= start + n - 1;) {
			const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);
			const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH - 1);

			// Check for overflow
			if (i1 >= INTERIOR_LENGTH || i2 >= INTERIOR_LENGTH)
				return false;

			// Make 2nd level node if necessary
			if (root_->ptrs[i1] == NULL) {
				Node* n = NewNode();
				if (n == NULL) return false;
				root_->ptrs[i1] = n;
			}

			// Make leaf node if necessary
			if (root_->ptrs[i1]->ptrs[i2] == NULL) {
				Leaf* leaf = reinterpret_cast<Leaf*>((*allocator_)(sizeof(Leaf)));
				if (leaf == NULL) return false;
				memset(leaf, 0, sizeof(*leaf));
				root_->ptrs[i1]->ptrs[i2] = reinterpret_cast<Node*>(leaf);
			}

			// Advance key past whatever is covered by this leaf node
			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
		}
		return true;
	}
};

//// Two-level radix tree
//template <int BITS>
//class TCMalloc_PageMap2 {
//private:
//	// Put 32 entries in the root and (2^BITS)/32 entries in each leaf.
//	static const int ROOT_BITS = 5; // 32位下前5位搞一个第一层的数组
//	static const int ROOT_LENGTH = 1 << ROOT_BITS;
//
//	static const int LEAF_BITS = BITS - ROOT_BITS; // 32位下后14位搞成第二层的数组
//	static const int LEAF_LENGTH = 1 << LEAF_BITS;
//
//	// Leaf node
//	struct Leaf { // 叶子就是后14位的数组
//		void* values[LEAF_LENGTH];
//	};
//
//	Leaf* root_[ROOT_LENGTH];             // 根就是前5位的数组
//public:
//	typedef uintptr_t Number;
//
//	//explicit TCMalloc_PageMap2(void* (*allocator)(size_t)) {
//	explicit TCMalloc_PageMap2() { // 直接把所有的空间都开好
//		memset(root_, 0, sizeof(root_));
//		PreallocateMoreMemory(); // 直接开2M的span*全开出来
//	}
//
//	void* get(Number k) const {
//		const Number i1 = k >> LEAF_BITS;
//		const Number i2 = k & (LEAF_LENGTH - 1);
//		if ((k >> BITS) > 0 || root_[i1] == NULL) {
//			return NULL;
//		}
//		return root_[i1]->values[i2];
//	}
//
//	void set(Number k, void* v) {
//		const Number i1 = k >> LEAF_BITS;
//		const Number i2 = k & (LEAF_LENGTH - 1);
//		ASSERT(i1 < ROOT_LENGTH);
//		root_[i1]->values[i2] = v;
//	}
//
//	// 确保从start开始往后的n页空间开好了
//	bool Ensure(Number start, size_t n) {
//		for (Number key = start; key <= start + n - 1;) {
//			const Number i1 = key >> LEAF_BITS;
//
//			// Check for overflow
//			if (i1 >= ROOT_LENGTH)
//				return false;
//
//			// 如果没开好就开空间
//			if (root_[i1] == NULL) {
//				static ObjectPool<Leaf>	leafPool;
//				Leaf* leaf = (Leaf*)leafPool.New();
//
//				memset(leaf, 0, sizeof(*leaf));
//				root_[i1] = leaf;
//			}
//
//			// Advance key past whatever is covered by this leaf node
//			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
//		}
//		return true;
//	}
//
//	// 提前开好空间，这里就把2M的直接开好
//	void PreallocateMoreMemory() {
//		// Allocate enough to keep track of all possible pages
//		Ensure(0, 1 << BITS);
//	}
//};
//
//// Single-level array
//template <int BITS>
//class TCMalloc_PageMap1 {
//private:
//	static const int LENGTH = 1 << BITS; // 数组要开的长度
//	void** array_; // 底层存放指针的数组
//
//public:
//	typedef uintptr_t Number;
//
//	explicit TCMalloc_PageMap1() {// 开空间
//		size_t size = sizeof(void*) << BITS;
//		size_t alignSize = SizeClass::_RoundUp(size, 1 << PAGE_SHIFT);
//		array_ = (void**)SystemAlloc(alignSize >> PAGE_SHIFT);
//		memset(array_, 0, sizeof(void*) << BITS);
//	}
//
//	// Return the current value for KEY.  Returns NULL if not yet set,
//	// or if k is out of range.
//	void* get(Number k) const { // 通过k来获取对应的指针
//		if ((k >> BITS) > 0) {
//			return NULL;
//		}
//		return array_[k];
//	}
//
//	// REQUIRES "k" is in range "[0,2^BITS-1]".
//	// REQUIRES "k" has been ensured before.
//	//
//	// Sets the value 'v' for key 'k'.
//	void set(Number k, void* v) { // 将v设置到k下标
//		array_[k] = v;
//	}
//};