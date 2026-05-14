#pragma once
#define PAGECACHE_
#ifdef PAGECACHE_
#include"Common.h"

// 分配器函数
inline void* PageMapAlloc(size_t size)
{
	//return ::operator new(size);

	size_t pages = (size + (1 << PAGE_SHIFT) - 1) >> PAGE_SHIFT;
	void* ptr = SystemAlloc(pages);
	if (ptr == nullptr)
	{
		throw std::bad_alloc();   // 与你的 SystemAlloc 失败行为一致
	}
	return ptr;
	// 如果需要释放，也要提供对应的释放函数，但基数树不需要释放（或由系统回收），暂可忽略
}

class PageCache
{
public:
	static PageCache* GetInstance()
	{
		return &_sInst;
	}

	// pc从_spanlist中拿出一个k页的span
	//Span* _NewSpan(size_t k);
	Span* NewSpan(size_t k);

	// 通过页地址找到span
	Span* MapObjectToSpan(void* obj);

	// 管理还回来的span
	void ReleaseSpanToPageCache(Span* span);

	void SafeSet(PageID start, size_t n, Span* span);
private:
	SpanList _spanLists[PAGE_NUM];	// pc的哈希

	// 哈希映射，用来快速通过页号找到对应span
	// std::unordered_map<PageID, Span*> _idSpanMap;
	TCMalloc_PageMap3<64 - PAGE_SHIFT> _idSpanMap;
	//TCMalloc_PageMap1<32 - PAGE_SHIFT> _idSpanMap;

	ObjectPool<Span> _spanPool;	// 创建span的对象池
public:
	// 解决死锁的第三种方法：锁需要设置为公有
	// ，当然也可以给成私有然后提供一个返回锁的接口
	std::mutex _pageMtx;			// pc锁

private:
	// 单例，去掉构造、拷构、拷赋
	PageCache():_idSpanMap(PageMapAlloc)
	{}

	PageCache(const PageCache& pc) = delete;
	PageCache& operator = (const PageCache& pc) = delete;

	static PageCache _sInst;	// 单例类 对象
};

#endif