#pragma once
#include"Common.h"
#define THREAD_CACHE_
#ifdef THREAD_CACHE_

class ThreadCache
{
public:
	// 线程申请size大小的空间
	void* Allocate(size_t size);

	// 回收线程中大小为size的obj空间
	void Deallocate(void* obj, size_t size);

	// ThreadCache中空间不够时，我们向CentralCache申请空间的接口
	void* FetchFromCentralCache(size_t index, size_t alignSize);

	// tc向cc归还空间块数过多的桶中的空间
	void ListTooLong(FreeList& list, size_t size);
private:
	FreeList _freelists[FREE_LIST_SUM];  // 哈希，每个桶表示一个自由链表
};

// TLS的全局对象的指针，这样每个线程都能有一个独立的全局对象
//static _declspec(thread) ThreadCache* pTLSThreadCache = nullptr;
// 主要要给static，不然多个.cpp包含的时候会出现链接错误
// 每个线程都会在最初的时候创建一个全局的pTLSThreadCache指针，相互独立
// 用这个全局指针进行的任何操作都不会对其他线程产生干扰。

static thread_local ThreadCache* pTLSThreadCache = nullptr;
#endif