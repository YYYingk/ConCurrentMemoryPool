#pragma once
#include"Common.h"
#define CENTRALCACHE_
#ifdef CENTRALCACHE_

class CentralCache
{
public:
	// 单例接口
	static CentralCache* GetInstance()
	{
		return &_sInst;
	}

	// cc从自己的_spanLists中为tc提供内存块
	size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size);
	// start和end表示cc提供的空间的开始和结尾，输出型参数
	// n表示tc需要多少块size内存
	// size表示单块大小
	// 返回值是cc实际提供的内存大小

	Span* GetOneSpan(SpanList& list, size_t size);

	// 将tc还回来的空间放入span
	void ReleaseListToSpans(void* start, size_t size);

private:
	// 单例，去掉构造、拷构、拷赋
	CentralCache()
	{}

	CentralCache(const CentralCache& copy) = delete;
	CentralCache& operator = (const CentralCache& copy) = delete;
private:
	// 哈希桶挂的一个个span
	SpanList _spanLists[FREE_LIST_SUM];
	// 饿汉模式创建一个CC类
	static CentralCache _sInst;
};

#endif