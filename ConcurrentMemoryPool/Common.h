#pragma once
#include<iostream>
#include<vector>
#include<cassert>
#include<thread>
#include<mutex>
#include<algorithm>
#include<Windows.h>
#include<unordered_map>
#include<atomic>
#include<cstdio>

using std::vector;
using std::memset;
using std::endl;
using std::cout;

static const size_t FREE_LIST_SUM = 208; // 自由链表个数
static const size_t MAX_BYTES = 256 * 1024; // Thread_Cache单次申请的最大字节数
static const size_t PAGE_NUM = 129; // span的最大管理页数
static const size_t PAGE_SHIFT = 13; // 一页多少位，这里给一页8KB，就是13位

typedef size_t PageID;

inline static void* SystemAlloc(size_t kpage)
{
#ifdef _WIN64

	void* ptr = VirtualAlloc(0, kpage << PAGE_SHIFT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

#else
	// 此处应写linux的函数接口
#endif
	if (ptr == nullptr) {
		throw std::bad_alloc();
	}

	return ptr;
}

inline static void SystemFree(void* ptr)
{
#ifdef _WIN64
	VirtualFree(ptr, 0, MEM_RELEASE);
#else
	// sbrk unmmap等
#endif

}

static void*& ObjNext(void* obj)
{
	return *(void**)obj;
}
class SizeClass; // 这里要声明一下，不然PageMap中用到了SizeClass会报错，或者直接将PageMap放到最后面引用

// 这里头文件要放到这，不然上面的函数ObjectPool中没有，就会报错
#include"ObjectPool.h"
#include"PageMap.h"

class FreeList {
public:
	// 删除桶中n个块，并把删除空间作为输出型参数返回
	void PopRange(void*& start, void*& end, size_t n)
	{
		// 删除块数不能超过已有数量
		assert(n <= _size);

		start = end = _freelist;

		for (size_t i = 0; i < n - 1; i++)
		{
			end = ObjNext(end);
		}

		_freelist = ObjNext(end);
		ObjNext(end) = nullptr;
		_size -= n;
	}

	// 获取当前桶有多少块内存空间
	size_t Size()
	{
		return _size;
	}

	void PushRange(void* start, void* end, size_t size)
	{
		ObjNext(end) = _freelist;
		_freelist = start;

		_size += size;
	}

	bool Empty()	// 拿来判断桶是否为空
	{
		return _freelist == nullptr;
	}

	void Push(void* obj)
	{	// 用来回收空间的
		assert(obj); // 插入非空空间

		ObjNext(obj) = _freelist;
		_freelist = obj;

		++_size;
	}

	void* Pop()
	{	// 用来提供空间的
		assert(_freelist);	// 提供空间的前提得有空间

		void* obj = _freelist;
		_freelist = ObjNext(obj);

		--_size;

		return obj;
	}

	size_t& MaxSize()
	{
		return _maxSize;
	}

private:
	void* _freelist = nullptr;
	size_t _maxSize = 1;
	// 当前自由链表申请未达到上限时，能申请的最大块空间是多少
	// 初始值为1，表示第一次申请的一块
	// 上限后此值作废

	// 当前自由链表有多少块空间
	size_t _size = 0;
};

class SizeClass
{
public:
	//static size_t _RoundUp(size_t size, size_t alignNum)
	//{
	//	size_t res = 0;
	//	if (size % alignNum != 0)
	//	{
	//		// 有余数多给个对齐
	//		res = (size / alignNum + 1) * alignNum;
	//	}
	//	else
	//	{
	//		// 没余数，本身能对齐
	//		res = size;
	//	}
	//	return res;

	//	return ((size + alignNum - 1) & ~(alignNum - 1));
	//}

	static size_t _RoundUp(size_t size, size_t alignNum)
	{									// alignNum是size对应分区的对齐数
		return ((size + alignNum - 1) & ~(alignNum - 1));
	}

	static size_t RoundUp(size_t size)
	{	// 计算对齐后字节数
		if (size <= 128) {
			// [1, 128] 8B
			return _RoundUp(size, 8);
		}
		else if (size <= 1024)
		{
			// [128+1, 1024] 16B
			return _RoundUp(size, 16);
		}
		else if (size <= 8 * 1024)
		{
			// [1024+1, 8*1024] 128B
			return _RoundUp(size, 128);
		}
		else if (size <= 64 * 1024)
		{
			// [8*1024+1, 64*1024] 1024B
			return _RoundUp(size, 1024);
		}
		else if (size <= 256 * 1024)
		{
			// [64*1024+1, 256*1024] 8 * 1024B
			return _RoundUp(size, 8 * 1024);
		}
		else
		{
			// 不可能的情况，这里通过那tc申请空间不会超过256KB
			return _RoundUp(size, 1 << PAGE_SHIFT);
		}
	}

	// 求size对应在哈希表中的下标
	static inline size_t _Index(size_t size, size_t align_shift)
	{							/*这里align_shift是指对齐数的二进制位数。比如size为2的时候对齐数
								为8，8就是2^3，所以此时align_shift就是3*/
		return ((size + (1 << align_shift) - 1) >> align_shift) - 1;
		// 这里_Index计算的是当前size所在区域的第几个下标，
		// 所以Index的返回值需要加上前面所有区域的哈希桶的个数
	}

	// 计算映射的哪一个自由链表桶
	static inline size_t Index(size_t size)
	{
		assert(size <= MAX_BYTES);

		// 每个区间有多少个链
		static int group_array[4] = { 16, 56, 56, 56 };
		if (size <= 128)
		{
		// [1,128] 8B -->8B就是2^3B，对应二进制位为3位
			return _Index(size, 3); // 3是指对齐数的二进制位位数，这里8B就是2^3B，所以就是3
		}
		else if (size <= 1024)
		{
		// [128+1,1024] 16B -->4位
			return _Index(size - 128, 4) + group_array[0];
		}
		else if (size <= 8 * 1024)
		{
		// [1024+1,8*1024] 128B -->7位
			return _Index(size - 1024, 7) + group_array[1] + group_array[0];
		}
		else if (size <= 64 * 1024)
		{
		// [8*1024+1,64*1024] 1024B -->10位
			return _Index(size - 8 * 1024, 10) + group_array[2] + 
				group_array[1] + group_array[0];
		}
		else if (size <= 256 * 1024)
		{
		// [64*1024+1,256*1024] 8 * 1024B  -->13位
			return _Index(size - 64 * 1024, 13) + group_array[3] +
				group_array[2] + group_array[1] + group_array[0];
		}
		else
		{
			assert(false);
		}
		return -1;
	}

	static size_t NumMoveSize(size_t size)
	{
		// 申请空间>0
		assert(size > 0);
		
		// MAX_BYTES单个块的最大空间，也就是256K
		int num = MAX_BYTES / size;
		// 除之后我们需要再控制大小一次
		if (num > 512) {
			// 此处我们不妨计算一下，单次申请8B
			// 256KB / 8B得到的是3w多的数，那这样就会
			// 造成空间浪费，所以我们该小一点
			num = 512;
		}

		if (num < 2) {
			// 同理如果单次申请256KB，除下来num=1，
			// 这样就太少了，如果线程要调四次256KB的
			// 我们将num改为2，就可以少调几次，但也不能太大
			num = 2;
		}

		// [2, 512]，这个阈值是一次批量移动多少个对象的上限制
		return num;
	}

	// 块页匹配算法
	static size_t NumMovePage(size_t size) // size表示一块的大小
	{
		// cc中没有span为tc提供内存块时，就需要向pc申请一块span，此时需要根据一块空间的大小来匹配
		// 出一个维护页空间较为合适的span，以保证span为size后不浪费或不足够
		// ，还要再频繁申请相同大小的span

		// nummovesize是算出tc向cc申请size大小的块时的单次最大申请次数
		size_t num = NumMoveSize(size);

		// num * size就是单次申请最大空间大小
		size_t npage = num * size;

		// PAGE_SHIFT表示一页要占多少位，一页8KB就是13位，这里右移13位
		// ，其实就是除以页大小，算出来就是单次申请最大空间有多少页
		npage >>= PAGE_SHIFT;

		// 如果算出来为0，那就直接给1页，比如说size为8B，而num为512
		// ，npage算出来就是4KB，那如果一页8KB，移位后就为0了
		// ，意思是半页的空间都够8B单次申请的最大空间了，但二进制中没有0.5，此处就给1
		if (npage == 0)
		{
			npage = 1;
		}

		return npage;
	}
};

struct Span
{
public:
	PageID _pageID = 0;		// 页号
	size_t _n = 0;			// 当前span管理的页的数据
	size_t _objSize = 0;		// span管理页被切分成的块有多大

	void* _freelist = nullptr;	// 每个span下面挂的小块的头节点
	size_t use_count = 0;	// 当前span分配出去了多个块空间

	Span* _prev = nullptr;	// 前置节点
	Span* _next = nullptr;	// 后置节点

	bool _isUse = false;	// 判断当前span是在cc还是pc中
};

class SpanList
{
public:
	// pc部分
	// 删除掉第一个span
	Span* PopFront()
	{
		Span* front = _head->_next;

		// 删掉这个span
		Erase(front);

		// 返回第一个span
		return front;
	}

	// 判是否有span
	bool Empty()
	{
		// 带头双向循环空的时候_head指向自己
		return _head == _head->_next;
	}
	
	void PushFront(Span* span)
	{
		Insert(Begin(), span);
	}

	// 头结点
	Span* Begin()
	{
		return _head->_next;
	}

	Span* End()
	{
		return _head;
	}

	// cc部分
	void Insert(Span* pos, Span* ptr)
	{
		// 在pos前插入ptr
		// 确保两个东西不为空
		assert(pos);
		assert(ptr);

		Span* prev = pos->_prev;

		prev->_next = ptr;
		ptr->_prev = prev;

		ptr->_next = pos;
		pos->_prev = ptr;
	}

	void Erase(Span* pos)
	{
		// 确保pos既不为空，也不是哨兵位
		assert(pos);
		assert(pos != _head);

		Span* prev = pos->_prev;
		Span* next = pos->_next;

		prev->_next = next;
		next->_prev = prev;

		// pos结点不用delete回收
		// pos结点的span还要回收下，不能直接删掉

		// 回收逻辑
	}

	SpanList()
	{
		// 哨兵位头节点的初始化
		_head = new Span;
		_head->_prev = _head;
		_head->_next = _head;
	}
	
private:
	Span* _head;   // 哨兵位置头结点
public:
	// 每个cc哈希桶的桶锁
	std::mutex _mtx;
};