#include "pch.h"
#include "CoreGlobal.h"
#include "ThreadManager.h"
#include "Memory.h"
#include "DeadLockProfiler.h"
#include "SocketUtils.h"
#include "SendBuffer.h"
#include "GlobalQueue.h"
#include "JobTimer.h"
#include "ConsoleLog.h"

ThreadManager*		GThreadManager = nullptr;
Memory*				GMemory = nullptr;
SendBufferManager*	GSendBufferManager = nullptr;
GlobalQueue*        GGlobalQueue = nullptr;
JobTimer*           GJobTimer = nullptr;

DeadLockProfiler*	GDeadLockProfiler = nullptr;
ConsoleLog*         GConsoleLogger = nullptr;

class CoreGlobal
{
public:
	CoreGlobal()
	{
		GThreadManager = new ThreadManager();
		GMemory = new Memory();
		GSendBufferManager = new SendBufferManager();
		GGlobalQueue = new GlobalQueue();
		GJobTimer = new JobTimer();
		GDeadLockProfiler = new DeadLockProfiler();
		GConsoleLogger = new ConsoleLog();
		SocketUtils::Init();
	}

	~CoreGlobal()
	{
		// 종료 순서: GMemory 가 모든 consumer 보다 늦게 destruct 되어야 함.
		// GGlobalQueue/GJobTimer 안의 Job 이 shared_ptr<Session> 등을 들고 있다가
		// 그 Job 이 destruct 되면 control block (GMemory pool 안) 을 _Decref 한다.
		// 기존 순서: GThreadManager → GMemory → ... → 큐 들 → 큐 안의 job destruct →
		//   이미 해제된 GMemory pool 접근 → access violation 발생.
		// 안전 순서: 1) 스레드 join → 2) 큐/매니저 → 3) GMemory 마지막.
		delete GThreadManager;       // 워커 스레드 join
		delete GJobTimer;            // 예약된 Job 정리 (shared_ptr 들 release)
		delete GGlobalQueue;         // 큐에 쌓인 Job 정리 (shared_ptr 들 release)
		delete GSendBufferManager;   // 송신 버퍼 정리
		delete GDeadLockProfiler;
		delete GConsoleLogger;
		delete GMemory;              // 모든 consumer 소멸 후 마지막
		SocketUtils::Clear();
	}
} GCoreGlobal;