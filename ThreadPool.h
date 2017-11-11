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
#include <list>
#include <queue>
#include <future>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <typeinfo>

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

			/**
			 * @brief Worker default constructor
			 *
			 * Default constructor is not applicable as the ThreadPool reference is mandatory
			 */
			Worker( void ) = delete;

		public:

			/**
			 * @brief Worker constructor
			 *
			 * ThreadPool reference is mandatory for Worker to operate
			 */
			Worker( ThreadPool * threadPool )
				:	/* Save ThreadPool reference pointer */
					mThreadPool( threadPool ),
					/* Initially the worker state is idle */
					mState( new Idle )
			{
				/* Run the task runner in new thread */
				this->mThread = std::thread( std::bind( & Worker::taskRunner, this ) );
			}

			/**
			 * @brief Worker copy constructor [DELETED]
			 *
			 * Worker is not copyable as the thread inside must be unique
			 */
			Worker( const Worker & ) = delete;

			/**
			 * @brief Worker move constructor [DEFAULT]
			 *
			 * Worker shall be moveable
			 */
			Worker( Worker && ) = default;

			/**
			 * @brief Thread pool worker destructor
			 *
			 * The destructor's goal is to request worker termination, wait for the worker to finish
			 * it's last task and then destroy itself.
			 */
			~Worker( void )
			{
				/* Wait for the thread to finish */
				if( ( this->mThread ).joinable() ) ( this->mThread ).join();
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

			template<typename STATE>
			bool inState( void ) const
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

			/**
			 * @brief Worker is ready
			 *
			 * If the worker is either in IDLE or RUNNING state, it is considered active
			 */
			bool isActive( void ) const
			{
				return( ( this->inState<Idle>() ) || ( this->inState<Running>() ) );
			}

			bool isTerminating( void ) const
			{
				return( this->inState<Terminating>() );
			}

			bool isShutDown( void ) const
			{
				return( this->inState<Shutdown>() );
			}

			/* 'External' commands */

			void run( void )
			{
				/* Lock the state to get thread safe access */
				std::unique_lock<std::mutex> stateLock( this->mStateMutex );

				( this->mState )->run( this );

				/* Unlock state */
				stateLock.unlock();
			}

			void wait( void )
			{
				/* Lock the state to get thread safe access */
				std::unique_lock<std::mutex> stateLock( this->mStateMutex );

				( this->mState )->wait( this );

				/* Unlock state */
				stateLock.unlock();
			}

			void terminate( void )
			{
				/* Lock the state to get thread safe access */
				std::unique_lock<std::mutex> stateLock( this->mStateMutex );

				( this->mState )->terminate( this );

				/* Unlock state */
				stateLock.unlock();
			}

		private:
		
			void idle( void )
			{
				/* Lock the state to get thread safe access */
				std::unique_lock<std::mutex> stateLock( this->mStateMutex );

				( this->mState )->idle( this );

				/* Unlock state */
				stateLock.unlock();
			}

			void shutdown( void )
			{
				/* Lock the state to get thread safe access */
				std::unique_lock<std::mutex> stateLock( this->mStateMutex );

				( this->mState )->shutdown( this );

				/* Unlock state */
				stateLock.unlock();
			}

			class State
			{
			public:
				virtual ~State( void ) = default;

				virtual void idle( Worker * worker ) {}
				virtual void wait( Worker * worker ) {}
				virtual void run( Worker * worker ) {}
				virtual void terminate( Worker * worker ) {}
				virtual void shutdown( Worker * worker ) {}
			};

			class Idle : public State
			{
			public:
				void run( Worker * worker ) override final
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << worker->getID() << "] IDLE -> RUNNING" << std::endl;
#endif
					/* Transit the state to Running */
					worker->mState = new Running();
				}

				void terminate( Worker * worker ) override final
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << worker->getID() << "] IDLE -> TERMINATING" << std::endl;
#endif
					/* Transit the state to Terminating */
					worker->mState = new Terminating();
				}
			};

			class Running : public State
			{
			public:
				void wait( Worker * worker ) override final
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << worker->getID() << "] RUNNING -> WAITING" << std::endl;
#endif
					/* Transit the state to Waiting */
					worker->mState = new Waiting();

					/* Wake up trimming thread */
					( worker->mThreadPool->mTrimmerWakeUp ).notify_all();
				}

				void idle( Worker * worker ) override final
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << worker->getID() << "] RUNNING -> IDLE" << std::endl;
#endif
					/* Transit the state to Idle */
					worker->mState = new Idle();
				}

				void terminate( Worker * worker ) override final
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << worker->getID() << "] RUNNING -> TERMINATING" << std::endl;
#endif
					/* Transit the state to Terminating */
					worker->mState = new Terminating();
				}
			};

			class Waiting : public State
			{
			public:
				void run( Worker * worker ) override final
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << worker->getID() << "] WAITING -> RUNNING" << std::endl;
#endif
					/* Transit the state to Running */
					worker->mState = new Running();

					/* Wake up trimming thread */
					( worker->mThreadPool->mTrimmerWakeUp ).notify_all();
				}
			};

			class Terminating : public State
			{
			public:
				void shutdown( Worker * worker ) override final
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << worker->getID() << "] TERMINATING -> SHUTDOWN" << std::endl;
#endif
					/* Transit the state to Waiting */
					worker->mState = new Shutdown();
				}
			};

			class Shutdown : public State
			{
			};

			void taskRunner( void )
			{
				/* Remain within infinite loop till terminated */
				while( !( this->inState<Terminating>() ) )
				{
					/* First of all check whether some workers should be removed. If so, this worker
					 * is the first among others which detects that so it should terminate itself. */
					if( this->mThreadPool->mWorkersToRemove > 0 )
					{
						/* As this worker is to be terminated, the overall amount must be lowered by one */
						this->mThreadPool->mWorkersToRemove--;

						/* Terminate itself */
						this->terminate();

						/* Break this loop iteration */
						break;
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
						 * wait() and run() methods - handled via the ThreadPool instance. So the worker state might be
						 * switched between running and waiting state */
						task();

						/* Once the task is finished, switch the worker to IDLE state */
						this->idle();
					}
				}

				/* Go to shutdown state */
				this->shutdown();

				/* Wake up trimming thread */
				( this->mThreadPool->mTrimmerWakeUp ).notify_all();
			}

		private:

			ThreadPool *		mThreadPool;

			mutable std::mutex	mStateMutex;

			State * 			mState;

			std::thread			mThread;
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
			:	/* Estimate number of threads based on the HW available */
				mThreadPoolSize( std::thread::hardware_concurrency() - 1 ),
				/* Amount of workers to be removed is initially zero. The value is calculated by trimming thread */
				mWorkersToRemove( 0 )
		{
			/* Run the task runner in new thread */
			this->mTrimmingThread = std::thread( std::bind( & ThreadPool::trim, this ) );
		}

		~ThreadPool( void )
		{
			/* First of all, terminate all workers */
			for( Core::ThreadPool::Worker & worker : this->mWorkers )
			{
				worker.terminate();
			}

			/* Notify all workers that it's time to recover from possible IDLE state
			 * and to do the termination */
			this->mWorkerWakeUp.notify_all();

			/* No (zero) workers are needed. */
			this->mThreadPoolSize = 0;

			/* Do the trimming */
			this->mTrimmerWakeUp.notify_one();

			/* Join trimming thread */
			( this->mTrimmingThread ).join();

			std::cout << "ThreadPool shut down." << std::endl;
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
			this->mWorkerWakeUp.notify_one();

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
		/* TODO: Rename the method to more self-describing name */
		void wait( void )
		{
			/* Lock the workers container to get thread safe access */
			std::unique_lock<std::mutex> workersListLock( this->mWorkersMutex );

			/* Call worker wait() method */
			( this->getWorker() ).wait();

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

			/* Call worker run() method */
			( this->getWorker() ).run();

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
		Core::ThreadPool::Worker & getWorker( void )
		{
			/* Get current thread ID => identify the thread in which the notifyWaiting
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

		/**
		 * @brief Task type definition
		 *
		 * Task type to be used in task queue.
		 */
		using TTask = std::function<void( void )>;

		struct WorkerRemovalPredicate
		{
			bool operator() ( const Worker & worker ) const
			{
				return( worker.isShutDown() );
			}
		};

		void trim( void )
		{
			while( true )
			{
				/* Lock the task queue to get thread safe access */
				std::unique_lock<std::mutex> workersListLock( this->mWorkersMutex );

				unsigned int nActiveWorkers = 0U;

				for( const Core::ThreadPool::Worker & worker : this->mWorkers )
				{
					if( worker.isActive() ) nActiveWorkers++;
				}

				/* There are less active workers than recommended -> add some */
				/* TODO: Is that a good approach to add more than one worker in single trimming iteration?
				 * Maybe just to add one worker per trimming iteration... */
				if( nActiveWorkers < this->mThreadPoolSize )
				{
					for( unsigned int i = 0; i < ( this->mThreadPoolSize - nActiveWorkers ); i++ )
					{
						std::cout << "Adding worker" << std::endl;

						( this->mWorkers ).emplace( ( this->mWorkers ).end(), this );
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
					this->mWorkerWakeUp.notify_one();
				}

				/* TODO: remove */
				std::cout << "Active workers = " << nActiveWorkers << ", Workers to remove = " << this->mWorkersToRemove << std::endl;

				/* Remove all the workers which match the worker removal predicate (are in ShutDown state) */
				( this->mWorkers ).remove_if( WorkerRemovalPredicate() );

				if( !( this->mWorkers ).empty() )
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

	private:

		/* TODO: Rework a little -> not to operate with the mutex but with std::unique_lock<std::mutex> ?
		 * Inspiration: https://stackoverflow.com/a/21900725/5677080 */
		mutable std::mutex			mTaskQueueMutex;

		/* TODO: The type of queue might be changed to std::priority_queue. The tasks can then be a pack of
		 * the task itself and it's priority */
		std::queue<TTask>			mTaskQueue;

		/**
		 * @brief Workers
		 *
		 * ThreadPool workers container - extended threads which perform the jobs
		 * std::list is a container that supports constant time insertion and removal of elements
		 * from anywhere in the container. This feature is used in adding/removal of new workers.
		 */
		mutable std::mutex			mWorkersMutex;

		std::list<Worker>			mWorkers;

		std::condition_variable		mWorkerWakeUp;

		std::thread					mTrimmingThread;

		std::condition_variable		mTrimmerWakeUp;

		/* TODO: The size parameter is now calculated during ThreadPool constuction. It might be valuable
		 * option to make it configurable somehow */
		std::atomic<unsigned int> 	mThreadPoolSize;

		std::atomic<unsigned int>	mWorkersToRemove;
	};
}
#endif /* THREADPOOL_THREADPOOL_H_ */
