#pragma once
#define OBJECT_POOL_
#ifdef OBJECT_POOL_
#include"Common.h"

template<class T>
class ObjectPool {
public:
	T* New(){
		// 1.
		//T* obj = nullptr;	// 最终返回的空间
		//if (_memory == nullptr) {	// memory没空间就开空间
		//	_memory = (char*)malloc(128 * 1024);
		//	if (_memory == nullptr) {
		//		throw std::bad_alloc(); // 抛异常
		//	}
		//}

		//obj = (T*)_memory;		// 给它一个T类型大小的空间
		//_memory += sizeof(T);	// _memory后移一个T类型的大小

		//return obj;

		// 这时候最重要的问题来了
		// 当128K的空间都分配完之后，如果再向当前内存池要空间，_memory应该怎么办？往下看

		// 2.
		T* obj = nullptr;	// 最终返回的空间
		// 此处我们将利用还到delete中的空间
		if (_freelist) {
			// 有回收的T大小的小块在此处可以重复利用
			// 拿到相当于指向下一个节点的指针
			void* next = *(void**)_freelist;
			obj = (T*)_freelist;
			_freelist = next;
			// 头删
		}
		else {
			// _memory的剩余空间小于T的大小时再开空间
			if (_remanentBytes < sizeof(T)) {

				_remanentBytes = 128 * 1024;	// 再开128K空间

				// 右移13位，就是除以8KB，也就是得到的是16，这里就表示申请16页
				_memory = (char*)SystemAlloc(_remanentBytes >> 13);
				if (_memory == nullptr) {
					throw std::bad_alloc(); // 抛异常
				}
			}
			obj = (T*)_memory;		// 给它一个T类型大小的空间
			// 1.
			//_memory += sizeof(T);	// _memory后移一个T类型的大小
			//_remanentBytes -= sizeof(T);

			// 2.
			// 此处就是我们解决T小于指针大小的方案
			size_t objSize = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
			_memory += objSize;
			_remanentBytes -= objSize;

		}

		// 在指定的“地址”上调用对应类型的构造函数
		new(obj) T;

		return obj;

		// 到这其实还有一个小问题，T如果小于一个指针的大小（4/8字节）该怎么办？
		// 这个问题现在先不管，先默认都是大于指针大小的，等一会解决。
	}

	void Delete(T* obj) {  // 回收还回来的小空间
		// 显示调用析构函数进行清理工作
		obj->~T();

		// 1.
		//if (_freelist == nullptr) {
		//	*(void**)obj = nullptr;
		//	_freelist = obj;
		//}
		//else {
		//	*(void**)obj = _freelist;
		//	_freelist = obj;
		//}

		// 2.
		// 上面两种情况合并为下方代码
		*(void**)obj = _freelist;
		_freelist = obj;
		// 那么此时这块空间都还可以重复利用，我们就在new中重新利用一下

		// 不过这里也会有和New中一样的问题，就是T类型的大小可能会小于指针的大小
		// 这里也是先搁置，等一会在New中一块解决
	}


private:
	char* _memory = nullptr;		// 指向内存块的指针，因为要进行+-运算故用char定义
	size_t _remanentBytes = 0;		// 大块内存存在切分过程中的剩余字节数
	void* _freelist = nullptr;		// 自由链表，用来连接归还的空闲空间
public:
	std::mutex _poolMtx;			// 防止ThreadCache中申请时申请到空指针
};

#endif