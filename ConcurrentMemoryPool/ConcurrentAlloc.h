#pragma once
#define CONCURRENTALLOC_
#ifdef CONCURRENTALLOC_
#include"ThreadCache.h"
#include"PageCache.h"

// 其实就是tcmalloc，线程调用这个函数申请空间
static void* ConcurrentAlloc(size_t size)
{
	// 如果申请空间超过256KB，就直接找下层的去要
	if (size > MAX_BYTES)
	{
		size_t alignSize = SizeClass::RoundUp(size); // 先按照页大小对齐
		size_t k = alignSize >> PAGE_SHIFT; // 算出来对齐之后需要多少页

		PageCache::GetInstance()->_pageMtx.lock(); // 对pc中的span进行操作，加锁
		Span* span = PageCache::GetInstance()->NewSpan(k); // 直接向pc要
		span->_objSize = size; // 统计大于256KB的页
		PageCache::GetInstance()->_pageMtx.unlock(); // 解锁

		void* ptr = (void*)(span->_pageID << PAGE_SHIFT); // 通过获得到的span来提供空间
		return ptr;
	}
	else // 申请空间小于256KB的就走原先的逻辑
	{
		/* 因为pTLSThreadCache是TLS的，每个线程都会有一个，且相互独立，所以不存在竞
		争pTLSThreadCache的问题，所以这里只需要判断一次就可以直接new，不存在线程安全问题*/
		if (pTLSThreadCache == nullptr)
		{
			// pTLSThreadCache = new ThreadCache; // 不用malloc
			// 此时就相当于每个线程都有了一个ThreadCache对象

			// 用定长内存池来申请空间
			static ObjectPool<ThreadCache> objPool; // 静态的，一直存在
			objPool._poolMtx.lock();
			pTLSThreadCache = objPool.New();
			objPool._poolMtx.unlock();
		}

		//cout << std::this_thread::get_id() << " " << pTLSThreadCache << endl;

		return pTLSThreadCache->Allocate(size);
	}
}

// 线程调用这个函数用来回收空间
static void ConcurrentFree(void* ptr)
{			/*这里第二个参数size后面会去掉的，
			这里只是为了让代码能跑才给的*/
	assert(ptr);

	// 通过ptr找到对应的span，因为前面申请空间的
	// 时候已经保证了维护的空间首页地址已经映射过了
	Span* span = PageCache::GetInstance()->MapObjectToSpan(ptr);
	size_t size = span->_objSize; // 通过映射来的span获取ptr所指空间大小

	// 通过size判断是不是大于256KB的，是了就走pc
	if (size > MAX_BYTES)
	{
		PageCache::GetInstance()->_pageMtx.lock(); // 记得加锁
		PageCache::GetInstance()->ReleaseSpanToPageCache(span); // 直接通过span释放空间
		PageCache::GetInstance()->_pageMtx.unlock(); // 解锁
	}
	else // 不是大于256KB的就走tc
	{
		pTLSThreadCache->Deallocate(ptr, size);
	}
}

#endif