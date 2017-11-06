/*
 * ThreadPool.h
 *
 *  Created on: 4. 10. 2017
 *      Author: martin
 */

#ifndef THREADPOOL_THREADPOOL_H_
#define THREADPOOL_THREADPOOL_H_

/* TODO: Remove this temporary development definition */
#ifndef THREADPOOL_IS_SINGLETON
#define THREADPOOL_IS_SINGLETON true
#endif

#define DEBUG_CONSOLE_OUTPUT true

/* Standard library inclusions */
#include <memory>
#include <future>
#include <atomic>
#include <list>
#include <queue>
#include <mutex>
#include <functional>
#include <condition_variable>

/* Boost inclusions */
#if THREADPOOL_IS_SINGLETON
	#include <boost/serialization/singleton.hpp>
#endif

/* Shared library support */
/* TODO: Improve symbol visibility mechanism to give the control to the target application */
#ifndef THREADPOOL_EXPORT
	#ifdef THREADPOOL_EXPORTS
		/* We are building this library */
		#define THREADPOOL_EXPORT __attribute__((visibility("default")))
	#else
	/* We are using this library */
		#define THREADPOOL_EXPORT __attribute__((visibility("default")))
	#endif
#endif

#ifndef THREADPOOL_NO_EXPORT
	#define THREADPOOL_NO_EXPORT __attribute__((visibility("hidden")))
#endif

namespace Core
{
	class THREADPOOL_EXPORT ThreadPool
#if THREADPOOL_IS_SINGLETON
		:	public boost::serialization::singleton<ThreadPool>
#endif
	{
	protected:
		class Worker
		{
		private:

			/** @enum mapper::State
			 *  @brief Thread pool worker status
			 */
			enum class State: unsigned int
			{
				IDLE,			/**< No task is being executed */
				RUNNING,		/**< Worker is currently running a task */
				WAITING,		/**< Worker is waiting */
				TERMINATING,	/**< Worker is about to be terminated */
				SHUTDOWN,		/**< Worker is shut down */
			};

#if DEBUG_CONSOLE_OUTPUT
			std::string showState( const State state )
			{
				switch( state )
				{
					case State::IDLE : return( "IDLE" ); break;
					case State::RUNNING : return( "RUNNING" ); break;
					case State::WAITING : return( "WAITING" ); break;
					case State::TERMINATING : return( "TERMINATING" ); break;
					case State::SHUTDOWN : return( "SHUTDOWN" ); break;
					default : return( "UNKNOWN" ); break;
				}
			}
#endif /* DEBUG_CONSOLE_OUTPUT */

			void setState( const State state )
			{
#if false
				if( state != this->mState )
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << this->getID() << "] " << this->showState( this->mState ) << "->" << this->showState( state ) << std::endl;
#endif /* DEBUG_CONSOLE_OUTPUT */

					switch( state )
					{
					/* New state TERMINATING is requested */
					case State::TERMINATING :
						if( this->mState != State::SHUTDOWN ) this->mState = state;
						break;

					/* New state SHUTDOWN is requested */
					case State::SHUTDOWN :
						if( this->mState == State::TERMINATING ) this->mState = state;
						break;

					default: this->mState = state;
						break;
					}
				}
#endif

				if( ( ( this->mState == State::TERMINATING ) && ( state == State::SHUTDOWN ) )
					|| ( ( this->mState != State::SHUTDOWN ) && ( state == State::TERMINATING ) )
					|| ( ( state < State::TERMINATING ) && ( this->mState != state  ) )

				  )
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << this->getID() << "] " << this->showState( this->mState ) << "->" << this->showState( state ) << std::endl;
#endif /* DEBUG_CONSOLE_OUTPUT */

					this->mState = state;
				}
			}

			State getState( void ) const
			{
				return( this->mState );
			}

		public:

			/**
			 * @brief Thread pool worker constructor
			 *
			 * Runs the worker's taskRunner() in the stand alone thread.
			 */
			Worker( ThreadPool * threadPool )
				:	mState( State::IDLE )
			{
				/* Run the task runner in new thread */
				this->mThread = std::thread( std::bind( & Worker::taskRunner, this, threadPool ) );
			}

			/**
			 * @brief Thread pool worker destructor
			 *
			 * The destructor's goal is to request worker termination, wait for the worker to finish
			 * it's last task and then destroy itself.
			 */
			~Worker( void )
			{
				this->terminate();

				std::thread::id ID = this->getID();

				/* Wait for the thread to finish */
				if( ( this->mThread ).joinable() ) ( this->mThread ).join();

				std::cout << "[Worker " << ID << "] Thread joined. Worker is destructed." << std::endl;

				return;
			}

			/* FIXME: The termination should be requested here only. It might not be possible to terminated the worker all the time.
			 * For example while the worker is executing the task and it's state is RUNNING, the worker must wait till the task is finished
			 * and terminate later on. */
			void terminate( void )
			{
				/* If the worker is not executing anything - is not in RUNNING nor in WAITING state - it can be terminated */
				this->setState( State::TERMINATING );
			}

			bool isTerminating( void ) { return( this->getState() == State::TERMINATING ); }

			bool isShutDown( void ) { return( this->getState() == State::SHUTDOWN ); }

			/**
			 * @brief Task is waiting notification
			 *
			 * Notifies the worker the task which is currently executed is waiting for some event.
			 */
			void wait( void ) { this->setState( State::WAITING ); }

			bool isWaiting( void ) { return( this->getState() == State::WAITING ); }

			/**
			 * @brief Task is running notification
			 *
			 * Notifies the worker the task which is currently continuing its execution.
			 */
			void run( void ) { this->setState( State::RUNNING ); }

			bool isRunning( void ) { return( this->getState() == State::RUNNING ); }

			/**
			 * @brief Worker is ready
			 *
			 * If the worker is either in IDLE or RUNNING state, it is considered active
			 */
			bool isActive( void )
			{
				return( ( this->getState() == State::IDLE )
				     || ( this->getState() == State::RUNNING ) );
			}

			/**
			 * @brief Worker thread ID
			 *
			 * Returns the thread::id of the thread in which worker is executing it's tasks
			 */
			std::thread::id getID( void ) const
			{
				return( ( this->mThread ).get_id() );
			}

		private:

			/**
			 * @brief Worker task runner
			 *
			 * This method is the hearth of the worker. Internally it is an infinite loop able to be terminated
			 * by appropriate worker state (State::TERMINATED). Until not terminated, it continuously
			 * checks the task queue for tasks to be executed. If there are any, it fetches the task out of the queue
			 * and executes it.
			 * Once there are no tasks waiting in the queue, it goes into idle mode and waits there until new tasks
			 * are available or the worker is terminated.
			 */
			void taskRunner( ThreadPool * threadPool )
			{
				/* Infinite loop */
				while( this->getState() != State::TERMINATING )
				{
					/* Lock the task queue to get thread safe access */
					std::unique_lock<std::mutex> taskQueueLock( threadPool->mTaskQueueMutex );

					/* Check the amount of tasks waiting in the queue. If there aren't any, go to IDLE state
					 * and wait there till some tasks are available or the worker is terminated */
					if( threadPool->getNumOfTasksWaiting() == 0 )
					{
						this->setState( State::IDLE );

						/* Wait for tasks to be available in the queue.
						 *
						 * Atomically releases lock, blocks the current executing thread. When unblocked, regardless of the reason,
						 * lock is reacquired and wait exits. The worker is blocked until the condition variable is notified by
						 * notify_one() or notify_all()
						 * OR
						 * the worker should be terminated.
						 */
						threadPool->mTasksAvailable.wait( taskQueueLock, [&]()
						{
							/* Predicate which returns ​false if the waiting should be continued */
							return( this->getState() == State::TERMINATING );

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
						this->setState( State::RUNNING );

						/* Fetch the task from task queue */
						TTask task = ( threadPool->mTaskQueue ).front();

						/* Remove fetched task */
						( threadPool->mTaskQueue ).pop();

						/* Unlock the task queue */
						taskQueueLock.unlock();

						/* Execute the task.
						 *
						 * From within the task, the worker might be notified the task is waiting for some event using
						 * wait() and run() methods - handled via the ThreadPool instance. So the worker state might be
						 * switched between State::RUNNING and State::WAITING */
						task();

						/* Once the task is finished, switch the worker to IDLE state */
						this->setState( State::IDLE );
					}
				}

				/* The worker is shut down */
				this->setState( State::SHUTDOWN );
			}

			State		mState;

			std::thread	mThread;

			bool 		mTerminated = false;
		};

#if THREADPOOL_IS_SINGLETON
	protected:
#else
	public:
#endif
		/**
		 * @brief ThreadPool constructor
		 *
		 * This constructor must be made protected in case of ThreadPool being a singleton.
		 */
		ThreadPool( void )
			:	mSize( std::thread::hardware_concurrency() - 1 ),
				/* TODO */
				mShutdown( false )
		{
			/* Run the task runner in new thread */
			this->mTrimmingThread = std::thread( std::bind( & ThreadPool::trim, this, this ) );
		}

		~ThreadPool( void )
		{
			std::cout << "Going to terminate all workers..." << std::endl;

			/* First of all, terminate all workers */
			for( auto & worker : this->mWorkers )
			{
				worker->terminate();
			}

			/* Notify all workers that it's time to recover from possible IDLE state
			 * and to do the termination */
			this->mTasksAvailable.notify_all();

			/* No (zero) workers are needed. */
			this->mSize = 0;

			/* Do the trimming */
			this->mPerformTrimming.notify_one();

			std::cout << "Going to join trimming thread." << std::endl;

			this->mShutdown = true;
			this->mPerformTrimming.notify_one();

			/* Join trimming thread */
			( this->mTrimmingThread ).join();

			std::cout << "Joined." << std::endl;

			std::cout << "ThreadPool destructor done." << std::endl;
		}

	public:

		/**
		 * @brief Get the amount of tasks waiting in task queue
		 */
		std::size_t getNumOfTasksWaiting( void ) const
		{
			return( ( this->mTaskQueue ).size() );
		}

		/**
		 * @brief Add function to be executed in the ThreadPool
		 *
		 * Add a function to be executed, along with any arguments for it
		 */
		template<typename FUNCTION, typename... ARGUMENTS>
		auto add( FUNCTION&& function, ARGUMENTS&&... arguments ) -> std::future<typename std::result_of<FUNCTION(ARGUMENTS...)>::type>
		{
			/* Deduce package task type from given function and its arguments */
			using TPackagedTask = std::packaged_task<typename std::result_of<FUNCTION(ARGUMENTS...)>::type()>;

			/* Make unique packaged task instance */
			std::shared_ptr<TPackagedTask> tTask = std::make_shared<TPackagedTask>( std::bind( std::forward<FUNCTION>( function ), std::forward<ARGUMENTS>( arguments )... ) );

			/* Get the future to return later */
			std::future<typename std::result_of<FUNCTION( ARGUMENTS... )>::type> returnFuture = tTask->get_future();

			/* Lock the task queue */
			std::unique_lock<std::mutex> taskQueueLock( this->mTaskQueueMutex );

			/* Emplace the task into the task queue */
			this->mTaskQueue.emplace( [tTask]() { (* tTask)(); } );

			/* Unlock the task queue */
			taskQueueLock.unlock();

			/* Let waiting workers know there is an available job */
			this->mTasksAvailable.notify_one();

			return returnFuture;
		}

		/**
		 * @brief Worker wait notification
		 *
		 * This method shall be used inside task function to inform the thread pool the task is
		 * waiting for some event and thus the worker is blocked and not processing the task.
		 * If all the workers are theoretically in such state it might lead into thread pool deadlock.
		 * This mechanism is used to prevent that.
		 */
		void wait( void )
		{
			/* Lock the workers container to get thread safe access */
			std::unique_lock<std::mutex> workersListLock( this->mWorkersMutex );

			/* Check wheter there is matching worker and is still valid */
			if( !( this->getWorker() ).expired() )
			{
				/* Call worker wait() method */
				( ( this->getWorker() ).lock() )->wait();
			}

			/* Unlock the workers container */
			workersListLock.unlock();
		}

		/**
		 * @brief Worker run notification
		 *
		 * This method shall be used inside task function to inform the thread pool the task is
		 * running again after some period of time spent in waiting mode.
		 */
		void run( void )
		{
			/* Lock the workers container to get thread safe access */
			std::unique_lock<std::mutex> workersListLock( this->mWorkersMutex );

			/* Check wheter there is matching worker and is still valid */
			if( !( this->getWorker() ).expired() )
			{
				/* Call worker run() method */
				( ( this->getWorker() ).lock() )->run();
			}

			/* Unlock the workers container */
			workersListLock.unlock();
		}

	protected:

		/**
		 * @brief Get current worker being used
		 *
		 * This method gets the current thread ID and searches for a worker in which scope
		 * the method has been executed.
		 */
		std::weak_ptr<Worker> getWorker( void ) const
		{
			/* Get current thread ID => identify the thread in which the notifyWaiting
			 * method was executed */
			std::thread::id currentThreadID = std::this_thread::get_id();

			/* Iterate through all the workers currently existing */
			for( std::shared_ptr<Worker> worker : this->mWorkers  )
			{
				/* If the thread ID of worker being examined matches current thread,
				 * notify the worker about task being in waiting mode */
				if( worker->getID() == currentThreadID )
				{
					return( worker );
				}
			}

			/* If no matching worker is found, return empty pointer. This will be recognized
			 * by weak_ptr's expire() method */
			return( std::weak_ptr<Worker>() );
		}

		/**
		 * @brief Task type definition
		 *
		 * Task type to be used in task queue.
		 */
		using TTask = std::function<void( void )>;

		void trim( ThreadPool * threadPool )
		{
			std::cout << "Starting trimming thread" << std::endl;

			while( !( this->mShutdown ) )
			{
				/* Lock the task queue to get thread safe access */
				std::unique_lock<std::mutex> workersListLock( threadPool->mWorkersMutex );

				unsigned int nActiveWorkers = 0;

				for( const auto & worker : this->mWorkers )
				{
					if( worker->isActive() ) nActiveWorkers++;
				}

				/* There are less active workers than recommended -> add some */
				if( nActiveWorkers < threadPool->mSize )
				{
					for( unsigned int i = 0; i < ( threadPool->mSize - nActiveWorkers ); i++ )
					{
						( this->mWorkers ).emplace( ( this->mWorkers ).end(), std::make_shared<Worker>( this ) );
					}
				}

				/* Remove all terminated workers */
				for( const auto & worker : this->mWorkers )
				{
					/* If worker is currently shutting down itself */
					if( worker->isTerminating() )
					{
						/* Block the thread till the worker is really shut down */
						while( !worker->isShutDown() );

						std::cout << "Trim removes worker... " << worker << std::endl;
						/* Remove the worker completely */
						( this->mWorkers ).remove( worker );

						std::cout << "Done." << worker << std::endl;
					}
				}

				/* Wait here till the condition variable is notified */
				threadPool->mPerformTrimming.wait( workersListLock );

				workersListLock.unlock();
			}
		}

	private:

		/* TODO: Rework a little -> not to operate with the mutex but with std::unique_lock<std::mutex> ?
		 * Inspiration: https://stackoverflow.com/a/21900725/5677080 */
		mutable std::mutex			mTaskQueueMutex;

		std::queue<TTask>			mTaskQueue;

		/* TODO: Rename the condition variable as the name does not reflect the real usage */
		std::condition_variable		mTasksAvailable;

		/**
		 * @brief Workers
		 *
		 * ThreadPool workers container - extended threads which perform the jobs
		 * std::list is a container that supports constant time insertion and removal of elements
		 * from anywhere in the container. This feature is used in adding/removal of new workers.
		 */
		using TWorkersContainer = std::list<std::shared_ptr<Worker>>;

		mutable std::mutex			mWorkersMutex;

		TWorkersContainer			mWorkers;

		std::thread					mTrimmingThread;

		std::condition_variable		mPerformTrimming;

		std::atomic<unsigned int> 	mSize;

		std::atomic<bool>			mShutdown;
	};
}
#endif /* THREADPOOL_THREADPOOL_H_ */
