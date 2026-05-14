#include"CentralCache.h"
#include"PageCache.h"

// 类外初始化
CentralCache CentralCache::_sInst;

size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size)
{
	// 获取size对应到哪个哈希槽
	size_t index = SizeClass::Index(size);

	// 对桶操作要加桶锁
	_spanLists[index]._mtx.lock();

	// 获取到一个管理空间非空的span
	Span* span = GetOneSpan(_spanLists[index], size);
	assert(span);			// span不为空
	assert(span->_freelist);// span管理的空间不为空

	// 起初都指向_freelist，让end不断往后走
	start = end = span->_freelist;
	// 函数实际返回值
	size_t actualNum = 1;

	// 在end的next不为空的前提下，让end走batchNum - 1步
	size_t i = 0;
	while (i < batchNum - 1 && ObjNext(end) != nullptr)
	{
		end = ObjNext(end);
		++actualNum;	// 记录end走了多少步
		++i;			// 控制条件
	}

	// 返回[start, end]后调整span的freelist指针
	span->_freelist = ObjNext(end);
	span->use_count += actualNum;	// 给tc分了多少就给useCount加多少

	// 返回的空间不要和原先Span的_freelist中的块相连
	// 我们将end后的内容截断
	ObjNext(end) = nullptr;

	// 释放锁
	_spanLists[index]._mtx.unlock();

	return actualNum;
}

Span* CentralCache::GetOneSpan(SpanList& list, size_t size)
{
	// 先在cc中找一下有没有管理空间非空的span
	Span* it = list.Begin();
	while (it != list.End())
	{
		// 找到管理空间非空的span
		if (it->_freelist != nullptr)
		{
			return it;
		}
		else
		{
			it = it->_next;
		}
	}

	// 解掉FetchRangeObj中的桶锁，让其他向该cc桶进行操作的线程能拿到锁
	list._mtx.unlock();

	// 走到这就是没找到

	// 将size转化成页数，以供pc提供一个合适的span
	size_t k = SizeClass::NumMovePage(size);

	// 解决死锁的第三种方法
	PageCache::GetInstance()->_pageMtx.lock();
	Span* span = PageCache::GetInstance()->NewSpan(k);
	span->_isUse = true;
	span->_objSize = size;
	PageCache::GetInstance()->_pageMtx.unlock();

	// 这里需要强转一下，因为_pageID是PageID类型，不能直接赋值给指针
	char* start = (char*)(span->_pageID << PAGE_SHIFT);
	char* end = (char*)(start + (span->_n << PAGE_SHIFT));

	// 开始切分span管理的空间，并将其放入span->_freelist
	span->_freelist = start;

	void* tail = start;
	start += size;

	// 链接各个块
	while (start < end)
	{
		ObjNext(tail) = start;
		start += size;
		tail = ObjNext(tail);
	}
	ObjNext(tail) = nullptr; // 记得要把最后一位置空

	// 切好span后，需要把span挂到cc对应下标的桶中去
	list._mtx.lock();	// 挂之前先加把锁
	list.PushFront(span);

	return span;
}

void CentralCache::ReleaseListToSpans(void* start, size_t size)
{
	// 先通过size找到对应桶
	size_t index = SizeClass::Index(size);

	// 下面要对cc的span操作，我们先加锁
	_spanLists[index]._mtx.lock();

	while (start)
	{
		// 记录下start下一位
		void* next = ObjNext(start);

		// 找到span
		Span* span = PageCache::GetInstance()->MapObjectToSpan(start);

		// 把当前块插入到对应span中
		ObjNext(start) = span->_freelist;
		span->_freelist = start;

		// 还回来的空间，对应usecnt要--
		span->use_count--;
		if (span->use_count == 0)
		{
			// 先将span从cc中去掉
			_spanLists[index].Erase(span);
			span->_freelist = nullptr;
			span->_next = nullptr;
			span->_prev = nullptr;

			// 归还span，解掉当前桶锁
			_spanLists[index]._mtx.unlock();

			// 交给pc去管理
			PageCache::GetInstance()->_pageMtx.lock();
			PageCache::GetInstance()->ReleaseSpanToPageCache(span);
			PageCache::GetInstance()->_pageMtx.unlock();

			// 完毕后加上桶锁
			_spanLists[index]._mtx.lock();
		}

		// 换下一个
		start = next;
	}

	_spanLists[index]._mtx.unlock();
}