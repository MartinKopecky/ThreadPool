/*
 * ThreadPool.cpp
 *
 *  Created on: 6. 12. 2017
 *      Author: martin
 */

/* Standard library inclusions */
#include <iostream>
#include <functional>
#include <typeinfo>
#include <algorithm>

/* Project specific inclusions */
#include "ThreadPool.h"

namespace Core
{
	ThreadPool::Worker::Worker( ThreadPool * threadPool )
		:	/* Save ThreadPool reference pointer */
			mThreadPool( threadPool ),
			/* Initially the worker state is idle */
			mState( new Idle )
	{
		/* Run the task runner in new thread */
		this->mThread = std::thread( std::bind( & Worker::taskRunner, this ) );
	}

	ThreadPool::Worker::~Worker( void )
	{
		/* Wait for the thread to finish */
		if( ( this->mThread ).joinable() ) ( this->mThread ).join();
	}

	std::thread::id ThreadPool::Worker::getID( void ) const
	{
		return( ( this->mThread ).get_id() );
	}

	template<typename STATE>
	bool ThreadPool::Worker::inState( void ) const
	{
		/* Lock the state to get thread safe access */
		std::unique_lock<std::mutex> stateLock( this->mStateMutex );

		/* Evaluate whether the worker is in given state or not */
		bool retval = ( typeid( (* this->mState ) ) == typeid( STATE ) );

		/* It is done with the state, so unlock it */
		stateLock.unlock();

		/* Return the value */
		return( retval );
	}

	bool ThreadPool::Worker::isActive( void ) const
	{
		return( ( this->inState<Idle>() ) || ( this->inState<Running>() ) );
	}

	bool ThreadPool::Worker::isShutDown( void ) const
	{
		return( this->inState<Shutdown>() );
	}

	void ThreadPool::Worker::run( void )
	{
		/* Lock the state to get thread safe access */
		std::unique_lock<std::mutex> stateLock( this->mStateMutex );

		( this->mState )->run( this );

		/* Unlock state */
		stateLock.unlock();
	}

	void ThreadPool::Worker::blocked( void )
	{
		/* Lock the state to get thread safe access */
		std::unique_lock<std::mutex> stateLock( this->mStateMutex );

		( this->mState )->blocked( this );

		/* Unlock state */
		stateLock.unlock();
	}

	void ThreadPool::Worker::terminate( void )
	{
		/* Lock the state to get thread safe access */
		std::unique_lock<std::mutex> stateLock( this->mStateMutex );

		( this->mState )->terminate( this );

		/* Unlock state */
		stateLock.unlock();
	}

	bool ThreadPool::Worker::terminate_if( bool request )
	{
		if( request )
		{
			this->terminate();

			return( true );
		}
		else
		{
			return( false );
		}
	}

	void ThreadPool::Worker::idle( void )
	{
		/* Lock the state to get thread safe access */
		std::unique_lock<std::mutex> stateLock( this->mStateMutex );

		( this->mState )->idle( this );

		/* Unlock state */
		stateLock.unlock();
	}

	/**
	 * @brief Worker shutdown
	 *
	 * Thread safe method to set the worker state to SHUTDOWN state
	 */
	void ThreadPool::Worker::shutdown( void )
	{
		/* Lock the state to get thread safe access */
		std::unique_lock<std::mutex> stateLock( this->mStateMutex );

		( this->mState )->shutdown( this );

		/* Unlock state */
		stateLock.unlock();
	}

	void ThreadPool::Worker::taskRunner( void )
	{
		/* Remain within infinite loop till terminated */
		while( !( this->inState<Terminating>() ) )
		{
			/* First of all check whether some workers should be removed. If so, this worker
			 * is the first among others which detects that so it should terminate itself. */
			if( this->terminate_if( this->mThreadPool->mTrimmer->shouldTerminate() ) )
			{
#if DEBUG_CONSOLE_OUTPUT
				std::cout << "Worker " << this->getID() << " is going to remove itself as the pool is oversubscribed." << std::endl;
#endif
				this->mThreadPool->mTrimmer->confirmTermination();
			}

			/* Lock the task queue to get thread safe access */
			std::unique_lock<std::mutex> taskQueueLock( this->mThreadPool->mTaskQueueMutex );

			/* Check the amount of tasks waiting in the queue. If there aren't any, go to IDLE state
			 * and wait there till some tasks are available or the worker is terminated */
			if( this->mThreadPool->getNumOfTasksWaiting() == 0 )
			{
				/* Go to idle state */
				this->idle();

				/* Wait for tasks to be available in the queue.
				 *
				 * Atomically releases lock, blocks the current executing thread. When unblocked, regardless of the reason,
				 * lock is reacquired and wait exits. The worker is blocked until the condition variable is notified by
				 * notify_one() or notify_all()
				 * OR
				 * the worker should be terminated.
				 */
				this->mThreadPool->mWorkerWakeUp.wait( taskQueueLock, [&]()
				{
					/* Predicate which returns ​false if the waiting should be continued */
					return( this->inState<Terminating>() );
				} );

				/* Unlock the task queue */
				taskQueueLock.unlock();

				/* Once the waiting for the task or termination comes, break the iteration.
				 * If the termination is requested, the infinite loop is exited and the thread gets finalized. */
				break;
			}
			/* If there are some tasks to execute and the worker is not terminated, fetch the task and execute it */
			else
			{
				/* It is about to execute the task so the worker is running again */
				this->run();

				/* Fetch the task from task queue */
				TTask task = ( this->mThreadPool->mTaskQueue ).front();

				/* Remove fetched task */
				( this->mThreadPool->mTaskQueue ).pop();

				/* Unlock the task queue */
				taskQueueLock.unlock();

				/* Execute the task.
				 *
				 * From within the task, the worker might be notified the task is waiting for some event using
				 * blocked() and run() methods - handled via the ThreadPool instance. So the worker state might be
				 * switched between running and waiting state */
				task();

				/* Once the task is finished, switch the worker to IDLE state */
				this->idle();
			}
		}

		/* Go to shutdown state */
		this->shutdown();

		/* Notify trimmer it should do it's job */
		this->mThreadPool->mTrimmer->notify();
	}

	void ThreadPool::Worker::Idle::run( Worker * worker )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "[Worker " << worker->getID() << "] IDLE -> RUNNING" << std::endl;
#endif
		/* Transit the state to Running */
		worker->mState = new Running();
	}

	void ThreadPool::Worker::Idle::terminate( Worker * worker )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "[Worker " << worker->getID() << "] IDLE -> TERMINATING" << std::endl;
#endif
		/* Transit the state to Terminating */
		worker->mState = new Terminating();
	}

	void ThreadPool::Worker::Running::blocked( Worker * worker )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "[Worker " << worker->getID() << "] RUNNING -> BLOCKED" << std::endl;
#endif
		/* Transit the state to Blocked */
		worker->mState = new Blocked();

		/* Notify trimmer it's time to work */
		worker->mThreadPool->mTrimmer->notify();
	}

	void ThreadPool::Worker::Running::idle( Worker * worker )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "[Worker " << worker->getID() << "] RUNNING -> IDLE" << std::endl;
#endif
		/* Transit the state to Idle */
		worker->mState = new Idle();
	}

	void ThreadPool::Worker::Running::terminate( Worker * worker )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "[Worker " << worker->getID() << "] RUNNING -> TERMINATING" << std::endl;
#endif
		/* Transit the state to Terminating */
		worker->mState = new Terminating();
	}

	void ThreadPool::Worker::Blocked::run( Worker * worker )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "[Worker " << worker->getID() << "] BLOCKED -> RUNNING" << std::endl;
#endif
		/* Transit the state to Running */
		worker->mState = new Running();

		/* Notify trimmer it's time to work */
		worker->mThreadPool->mTrimmer->notify();
	}

	void ThreadPool::Worker::Terminating::shutdown( Worker * worker )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "[Worker " << worker->getID() << "] TERMINATING -> SHUTDOWN" << std::endl;
#endif
		/* Transit the state to Blocked */
		worker->mState = new Shutdown();
	}

	ThreadPool::TrimStrategy::ITrimStrategy::ITrimStrategy( ThreadPool * threadPool )
		:	/* Estimate number of threads based on the HW available */
			mThreadPoolSize( std::thread::hardware_concurrency() - 1 ),
			/* Amount of workers to be removed is initially zero. The value is calculated by trimming thread */
			mWorkersToRemove( 0U )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "Constructing trimming strategy. Starting the thread..." << std::endl;
#endif
		/* Run the task runner in new thread */
		this->mTrimmingThread = std::thread( std::bind( & ITrimStrategy::trim, this, threadPool ) );
	}

	ThreadPool::TrimStrategy::ITrimStrategy::~ITrimStrategy( void )
	{
		/* Join trimming thread */
		( this->mTrimmingThread ).join();
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "Trimming strategy destructed." << std::endl;
#endif
	}

	void ThreadPool::TrimStrategy::ITrimStrategy::setTrimTarget( unsigned int nWorkers )
	{
		this->mThreadPoolSize = nWorkers;
	}

	void ThreadPool::TrimStrategy::ITrimStrategy::notify( void )
	{
		/* Do the trimming */
		this->mTrimmerWakeUp.notify_one();
	}

	bool ThreadPool::TrimStrategy::ITrimStrategy::shouldTerminate( void ) const
	{
		return( this->mWorkersToRemove > 0U );
	}

	void ThreadPool::TrimStrategy::ITrimStrategy::confirmTermination( void )
	{
		if( this->mWorkersToRemove > 0U )
			this->mWorkersToRemove--;
	}

	void ThreadPool::TrimStrategy::DefensiveTrim::trim( ThreadPool * threadPool )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "Starting DEFENSIVE trimming strategy" << std::endl;
#endif
	}

	void ThreadPool::TrimStrategy::AggresiveTrim::trim( ThreadPool * threadPool )
	{
#if DEBUG_CONSOLE_OUTPUT
		std::cout << "Starting AGGRESIVE trimming strategy" << std::endl;
#endif
		while( true )
		{
			/* Lock the task queue to get thread safe access */
			std::unique_lock<std::mutex> workersListLock( threadPool->mWorkersMutex );

			unsigned int nActiveWorkers = std::count_if( ( threadPool->mWorkers ).begin(), ( threadPool->mWorkers ).end(), [&]( const Worker & worker ){ return( worker.isActive() ); } );

			/* There are less active workers than recommended -> add some */
			/* TODO: Is that a good approach to add more than one worker in single trimming iteration?
			 * Maybe just to add one worker per trimming iteration... */
			if( nActiveWorkers < this->mThreadPoolSize )
			{
				for( unsigned int i = 0; i < ( this->mThreadPoolSize - nActiveWorkers ); i++ )
				{
					/* Construct in-place new worker at the end of the list */
					( threadPool->mWorkers ).emplace_back( threadPool );
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "New Worker added." << std::endl;
#endif
				}
			}

			/* TODO: General approach is to keep mThreadPoolSize number of active workers while the others are blocked.
			 * Maybe it would be better strategy always keep one worker active, not quite many if the thread pool size is high.
			 * Both strategies might be available as option... */

			/* If there are more active workers than defined calculate the amount of workers to be terminated. */
			this->mWorkersToRemove = ( nActiveWorkers > this->mThreadPoolSize ) ? ( nActiveWorkers - this->mThreadPoolSize ) : 0U;

			if( this->mWorkersToRemove > 0 )
			{
				/* Notify some worker which is waiting in idle state to possibly remove itself */
				threadPool->mWorkerWakeUp.notify_one();
			}

			/* TODO: remove */
#if DEBUG_CONSOLE_OUTPUT
			std::cout << "Active workers = " << nActiveWorkers << ", Workers to remove = " << this->mWorkersToRemove << std::endl;
#endif
			/* Remove all the workers which match the worker removal predicate (are in ShutDown state) */
			( threadPool->mWorkers ).remove_if( [&]( const Worker & worker ){ return( worker.isShutDown() ); } );

			if( !( threadPool->mWorkers ).empty() )
			{
				/* Wait here till the condition variable is notified */
				this->mTrimmerWakeUp.wait( workersListLock );

				/* Unlock workers list */
				workersListLock.unlock();
			}
			else
			{
				/* Unlock workers list */
				workersListLock.unlock();

				/* Break trimming loop */
				break;
			}
		}
	}

	std::unique_ptr<ThreadPool::TrimStrategy::ITrimStrategy> ThreadPool::TrimStrategy::create( const Type which, ThreadPool * threadPool )
	{
		switch( which )
		{
		default:
		case Type::DEFENSIVE : return( std::make_unique<DefensiveTrim>( threadPool ) ); break;
		case Type::AGGRESIVE : return( std::make_unique<AggresiveTrim>( threadPool ) ); break;
		}
	}

#if THREADPOOL_IS_SINGLETON
	ThreadPool & ThreadPool::instance( void )
	{
		/* Since it is a static variable, if the class has already been created
		 * it won't be created again. In C++11 it is thread-safe */
		static ThreadPool threadPoolInstance;

		/* Return a reference to unique thread pool instance */
		return( threadPoolInstance );
	}
#endif

	ThreadPool::ThreadPool( void )
		:	/* Create the instance of trimming strategy */
			mTrimmer( TrimStrategy::create( TrimStrategy::Type::AGGRESIVE, this ) )
	{}

	ThreadPool::~ThreadPool( void )
	{
		/* TODO: This might not be needed any longer as this should be in scope of trimming strategy */
		/* First of all, terminate all workers */
		for( Core::ThreadPool::Worker & worker : this->mWorkers )
		{
			worker.terminate();
		}

		/* Notify all workers that it's time to recover from possible IDLE state
		 * and to do the termination */
		this->mWorkerWakeUp.notify_all();

		/* Set the trimming target to have no workers any more */
		this->mTrimmer->setTrimTarget( 0U );

		/* Perform trimming action */
		this->mTrimmer->notify();

#if DEBUG_CONSOLE_OUTPUT
		std::cout << "ThreadPool shut down." << std::endl;
#endif
	}

	void ThreadPool::notifyBlockedTask( void )
	{
		/* Lock the workers container to get thread safe access */
		std::unique_lock<std::mutex> workersListLock( this->mWorkersMutex );

		/* Call worker wait() method */
		( this->getWorker() ).blocked();

		/* Unlock the workers container */
		workersListLock.unlock();
	}

	/**
	 * @brief Task running notification
	 *
	 * This method shall be used inside task function to inform the thread pool the task is
	 * running again after some period of time spent in blocked mode.
	 */
	void ThreadPool::notifyRunningTask( void )
	{
		/* Lock the workers container to get thread safe access */
		std::unique_lock<std::mutex> workersListLock( this->mWorkersMutex );

		/* Call worker run() method */
		( this->getWorker() ).run();

		/* Unlock the workers container */
		workersListLock.unlock();
	}

	unsigned int ThreadPool::getNumOfTasksWaiting( void ) const
	{
		/* Converting size_t to unsigned int is potentially dangerous as
		 * size_t might me bigger and would cause overflow. But in dimensions
		 * used here it is not a big issue. We can hardly have more than 2^32 tasks
		 * waiting. */
		return( static_cast<unsigned int>( ( this->mTaskQueue ).size() ) );
	}

	ThreadPool::Worker & ThreadPool::getWorker( void )
	{
		/* Get current thread ID => identify the thread in which the notifyBlocked
		 * method was executed */
		std::thread::id currentThreadID = std::this_thread::get_id();

		/* Iterate through all the workers currently existing */
		for( Core::ThreadPool::Worker & worker : this->mWorkers  )
		{
			/* If the thread ID of worker being examined matches current thread,
			 * notify the worker about task being in waiting mode */
			if( worker.getID() == currentThreadID )
			{
				return( worker );
			}
		}

		/* Once the execution reaches this section, no matching worker has been found.
		 * This is considered as runtime error */
		throw std::runtime_error( "No worker found." );
	}
}
