#include"PageCache.h"

PageCache PageCache::_sInst;

//Span* PageCache::NewSpan(size_t k)
//{
//	_pageMtx.lock();
//	Span* res = _NewSpan(k);
//	_pageMtx.unlock();
//
//	return res;
//}

// pc从_spanlist中拿出一个k页的span
Span* PageCache::NewSpan(size_t k)
{
	// 申请页数一定是在[1, PAGE_NUM - 1]这个范围内的
	assert(k > 0);

	// 如果单次申请的页数超过128页时才需要向os申请，如果没有超过128页的还可以向pc申请
	if (k > PAGE_NUM - 1)
	{
		void* ptr = SystemAlloc(k); // 直接向os申请
		//Span* span = new Span; // 开一个新的span，用来管理新的空间
		Span* span = _spanPool.New(); // 用定长内存池开空间

		span->_pageID = ((PageID)ptr >> PAGE_SHIFT); // 申请空间的对应页号
		span->_n = k; // 申请了多少页

		// 把这个span管理的首页映射到哈希中，后面在删除这个span的时候能找到就行
		_idSpanMap.Ensure(span->_pageID, 1);
		_idSpanMap.set(span->_pageID, span);
		// 不需要把这个span让pc管理，pc只能管小于128页的span

		return span;
	}

	//GetInstance()->_pageMtx.lock();
	// 1、k号桶中有span
	if (!_spanLists[k].Empty())
	{
		// 直接返回第一个span
		Span* span = _spanLists[k].PopFront();

		// 记录分配出去的span管理的页号，和其地址的映射关系
		for (PageID i = 0; i < span->_n; i++)
		{
			_idSpanMap.Ensure(span->_pageID + i, 1);
			_idSpanMap.set(span->_pageID + i, span);
		}
		return span;
	}

	// 2、k号桶中没有span，但后面的桶有span
	for (int i = k + 1; i < PAGE_NUM; i++)
	{
		if(!_spanLists[i].Empty())
		{
			Span* nSpan = _spanLists[i].PopFront();

			// 开切

			Span* kSpan = _spanPool.New();

			// 分一个k页的span
			kSpan->_pageID = nSpan->_pageID;
			kSpan->_n = k;

			// 和一个 n - k 页的span
			nSpan->_pageID += k;
			nSpan->_n -= k;

			// n - k页的放回对应哈希桶
			_spanLists[nSpan->_n].PushFront(nSpan);

			// 再把n-k页的span边缘页映射一下，方便后续融合
			_idSpanMap.Ensure(nSpan->_pageID, 1);
			_idSpanMap.set(nSpan->_pageID, nSpan);
			_idSpanMap.Ensure(nSpan->_pageID + nSpan->_n - 1, 1);
			_idSpanMap.set(nSpan->_pageID + nSpan->_n - 1, nSpan);

			// 记录分配出去的span管理的页号，和其地址的映射关系
			for (PageID i = 0; i < kSpan->_n; i++)
			{
				_idSpanMap.Ensure(kSpan->_pageID + i, 1);
				_idSpanMap.set(kSpan->_pageID + i, kSpan);
			}

			return kSpan;
		}
	}
	
	// 3、k号和后面都没span，直接向系统申请128页的span
	
	// 直接向系统申请128页的span
	void* ptr = SystemAlloc(PAGE_NUM - 1);	// PAGE_NUM为129

	// 开一个新的span用来维护这块空间
	Span* bigSpan = _spanPool.New();

	// 只需要修改_pageID和_n即可
	// ，系统调用接口申请空间的时候一定能保证申请的空间是对齐的
	bigSpan->_pageID = ((PageID)ptr) >> PAGE_SHIFT;
	bigSpan->_n = PAGE_NUM - 1;

	// 将这个span放入到对应哈希桶中
	_spanLists[PAGE_NUM - 1].PushFront(bigSpan);

	// 递归再次申请k页的span，这次递归一定会去第2种的逻辑
	return NewSpan(k);
}

// 页地址找span
Span* PageCache::MapObjectToSpan(void* obj)
{
	// 通过块地址找到页号
	PageID id = (((PageID)obj) >> PAGE_SHIFT);

	//// 此处用智能锁
	//std::unique_lock<std::mutex> lc(_pageMtx);

	// 通过哈希找到页号对应span
	auto ret = _idSpanMap.get(id);

	if (ret != nullptr)
	{
		return (Span*)ret;
	}
	else
	{
		assert(false);
		return nullptr;
	}
}

void PageCache::ReleaseSpanToPageCache(Span* span)
{
	// 通过span判断释放的空间页数是否大于128页，如果大于就归还给os
	if (span->_n > PAGE_NUM - 1)
	{
		void* ptr = (void*)(span->_pageID << PAGE_SHIFT);	// 获取到要释放的地址
		SystemFree(ptr);	// 直接调用系统接口释放内存空间
		_spanPool.Delete(span);	// 释放掉span

		return;
	}

	// -----------页数小于128--------------
	// 向左合并
	while (1)
	{
		PageID leftID = span->_pageID - 1;
		auto ret = _idSpanMap.get(leftID);

		// 没有相邻span，停止合并
		if (ret == nullptr)
		{
			break;
		}

		Span* leftSpan = (Span*)ret;  // 相邻span
		// 相邻span在cc中，停止合并
		if (leftSpan->_isUse == true)
		{
			break;
		}

		// 相邻span与当期span合并后超过128页，停止合并
		if (leftSpan->_n + span->_n > PAGE_NUM - 1)
		{
			break;
		}

		// 当前span与相邻span进行合并
		span->_pageID = leftSpan->_pageID;
		span->_n += leftSpan->_n;

		_spanLists[leftSpan->_n].Erase(leftSpan);
		_spanPool.Delete(leftSpan);
	}

	// 向右合并
	while (1)
	{
		PageID rightID = span->_pageID + span->_n;
		auto ret = _idSpanMap.get(rightID);

		// 没有相邻span，停止合并
		if (ret == nullptr)
		{
			break;
		}

		Span* rightSpan = (Span*)ret;
		// 相邻span在cc中，停止合并
		if (rightSpan->_isUse == true)
		{
			break;
		}

		// 相邻span与当期span合并后超过128页，停止合并
		if (rightSpan->_n + span->_n > PAGE_NUM - 1)
		{
			break;
		}

		// 当前span与相邻span进行合并
		span->_n += rightSpan->_n;	// 往右就不用该span->pageID了
									// 因为右边会和span拼在一起

		// 把桶内的span删了
		_spanLists[rightSpan->_n].Erase(rightSpan);
		_spanPool.Delete(rightSpan);
	}

	// 合并完后，挂到对应桶中
	_spanLists[span->_n].PushFront(span);
	span->_isUse = false;

	// 映射当前span边缘页，后续还可以对这个span合并
	_idSpanMap.Ensure(span->_pageID, 1);
	_idSpanMap.set(span->_pageID, span);
	_idSpanMap.Ensure(span->_pageID + span->_n - 1, 1);
	_idSpanMap.set(span->_pageID + span->_n - 1, span);
}
