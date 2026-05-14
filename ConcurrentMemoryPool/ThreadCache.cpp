#include"ThreadCache.h"
#include"CentralCache.h"

// 线程申请size大小的空间
void* ThreadCache::Allocate(size_t size)
{
	assert(size <= MAX_BYTES); // 单次申请不超过256KB

	// size对齐后的字节数
	size_t alignSize = SizeClass::RoundUp(size);
	// size对应哈希表的哪个桶
	size_t index = SizeClass::Index(size);

	if (!_freelists[index].Empty())
	{
		// 链表不为空，可以直接从链表中获取空间
		return _freelists[index].Pop();
	}
	else
	{
		// 链表为空，得要让tc向cc申请空间
		return FetchFromCentralCache(index, alignSize);
	}
}

// 回收线程中大小为size的obj空间
void ThreadCache::Deallocate(void* obj, size_t size)
{
	assert(obj);
	assert(size <= MAX_BYTES);

	// 找到size对应的链表，此处size在后面代码是能保证是对齐的
	size_t index = SizeClass::Index(size);
	// 用自由链表来回收内存
	_freelists[index].Push(obj);

	if (_freelists[index].Size() >= _freelists[index].MaxSize())
	{
		ListTooLong(_freelists[index], size);
	}
}

// 向cc申请内存
void* ThreadCache::FetchFromCentralCache(size_t index, size_t alignSize)
{
	size_t batchNum = min(_freelists[index].MaxSize(), SizeClass::NumMoveSize(alignSize));
	// maxsize表示index位置的自由链表单次申请未到上限时，能够申请的最大块空间是多少
	// Nummovesize表示tc单次向cc申请alignsize大小的空间块的最大块数
	// 二者取小就是在控制本次要给tc提供内存块的数量
	// 也就是没到上限取maxsize，到了就取nummovesize

	if (batchNum == _freelists[index].MaxSize())
	{
		// 没到达上限的话，下次再申请可以多给一块
		_freelists[index].MaxSize()++;
		// 这就是慢开始反馈的核心
	}

	// ===========以上是慢开始反馈调节算法==============

	void* start = nullptr;
	void* end = nullptr;

	// 返回值为实际获取到的块数
	size_t actulNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, alignSize);
	
	assert(actulNum >= 1);

	if (actulNum == 1)
	{
		// 如果等于1，就直接返回start
		assert(start == end);
		return start;
	}
	else
	{
		// 若大于1，就要给tc对应位置插入[ObjNext(start), end]的空间
		_freelists[index].PushRange(ObjNext(start), end, actulNum - 1);

		// 给线程返回start所指空间
		return start;
	}
}

void ThreadCache::ListTooLong(FreeList& list, size_t size)
{
	void* start = nullptr;
	void* end = nullptr;

	list.PopRange(start, end, list.MaxSize());

	CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}
