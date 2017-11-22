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
/* TODO: Remove the dependency to Boost? Implement singleton inplace of ThreadPool? */
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

			/**
			 * @brief Worker state evaluation
			 *
			 * @returns true	{Worker is currently in STATE given as template parameter}
			 * @returns false	{Worker state is different to STATE}
			 */
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

			/**
			 * @brief Worker is terminating
			 *
			 * Worker is terminating itself so it cannot accept any further tasks.
			 */
			bool isTerminating( void ) const
			{
				return( this->inState<Terminating>() );
			}

			/**
			 * @brief Worker is shut down
			 *
			 * Worker termination is finished and the worker is ready to be destroyed.
			 */
			bool isShutDown( void ) const
			{
				return( this->inState<Shutdown>() );
			}

			/* 'External' commands */

			/**
			 * @brief Set Worker to RUNNING state
			 *
			 * Thread safe method to set the worker state to RUNNING
			 */
			void run( void )
			{
				/* Lock the state to get thread safe access */
				std::unique_lock<std::mutex> stateLock( this->mStateMutex );

				( this->mState )->run( this );

				/* Unlock state */
				stateLock.unlock();
			}

			/**
			 * @brief Set Worker to BLOCKED state
			 *
			 * Thread safe method to set the worker state to BLOCKED
			 */
			void blocked( void )
			{
				/* Lock the state to get thread safe access */
				std::unique_lock<std::mutex> stateLock( this->mStateMutex );

				( this->mState )->blocked( this );

				/* Unlock state */
				stateLock.unlock();
			}

			/**
			 * @brief Terminate the worker
			 *
			 * Thread safe method to set the worker state to TERMINATING state
			 */
			void terminate( void )
			{
				/* Lock the state to get thread safe access */
				std::unique_lock<std::mutex> stateLock( this->mStateMutex );

				( this->mState )->terminate( this );

				/* Unlock state */
				stateLock.unlock();
			}

			bool terminate_if( bool request )
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

		private:
		
			/**
			 * @brief Worker idle
			 *
			 * Thread safe method to set the worker state to IDLE state
			 */
			void idle( void )
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
			void shutdown( void )
			{
				/* Lock the state to get thread safe access */
				std::unique_lock<std::mutex> stateLock( this->mStateMutex );

				( this->mState )->shutdown( this );

				/* Unlock state */
				stateLock.unlock();
			}

			/**
			 * @brief State
			 *
			 * Generic worker state defining the interface to all concrete states.
			 * As the state transition methods here are not pure, the concrete state
			 * can implement just the transitions it needs. All the other state transitions
			 * would have the default behavior defined in here (do nothing by default)
			 */
			class State
			{
			public:
				virtual ~State( void ) = default;

				/**
				 * @brief Worker idle
				 *
				 * Default behavior for transition to IDLE. Should be overridden by concrete state
				 */
				virtual void idle( Worker * worker ) {}

				/**
				 * @brief Worker blocked
				 *
				 * Default behavior for transition to BLOCKED. Should be overridden by concrete state
				 */
				virtual void blocked( Worker * worker ) {}

				/**
				 * @brief Worker run
				 *
				 * Default behavior for transition to RUNNING. Should be overridden by concrete state
				 */
				virtual void run( Worker * worker ) {}

				/**
				 * @brief Worker terminate
				 *
				 * Default behavior for transition to TERMINATING. Should be overridden by concrete state
				 */
				virtual void terminate( Worker * worker ) {}

				/**
				 * @brief Worker shut down
				 *
				 * Default behavior for transition to SHUTDOWN. Should be overridden by concrete state
				 */
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
				void blocked( Worker * worker ) override final
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << worker->getID() << "] RUNNING -> BLOCKED" << std::endl;
#endif
					/* Transit the state to Blocked */
					worker->mState = new Blocked();

					/* Notify trimmer it's time to work */
					worker->mThreadPool->mTrimmer->notify();
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

			class Blocked : public State
			{
			public:
				void run( Worker * worker ) override final
				{
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "[Worker " << worker->getID() << "] BLOCKED -> RUNNING" << std::endl;
#endif
					/* Transit the state to Running */
					worker->mState = new Running();

					/* Notify trimmer it's time to work */
					worker->mThreadPool->mTrimmer->notify();
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
					/* Transit the state to Blocked */
					worker->mState = new Shutdown();
				}
			};

			class Shutdown : public State
			{};

			void taskRunner( void )
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

		private:

			ThreadPool *		mThreadPool;

			mutable std::mutex	mStateMutex;

			State * 			mState;

			std::thread			mThread;
		};

		class TrimStrategy
		{
		public:

			class ITrimStrategy
			{
			public:

				ITrimStrategy( ThreadPool * threadPool )
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

				virtual ~ITrimStrategy( void )
				{
					/* Join trimming thread */
					( this->mTrimmingThread ).join();
#if DEBUG_CONSOLE_OUTPUT
					std::cout << "Trimming strategy destructed." << std::endl;
#endif
				}

				void setTrimTarget( unsigned int nWorkers )
				{
					this->mThreadPoolSize = nWorkers;
				}

				void notify( void )
				{
					/* Do the trimming */
					this->mTrimmerWakeUp.notify_one();
				}

				bool shouldTerminate( void ) const
				{
					return( this->mWorkersToRemove > 0U );
				}

				void confirmTermination( void )
				{
					if( this->mWorkersToRemove > 0U )
						this->mWorkersToRemove--;
				}

			protected:
				struct WorkerRemovalPredicate
				{
					bool operator() ( const Worker & worker ) const
					{
						return( worker.isShutDown() );
					}
				};

				virtual void trim( ThreadPool * threadPool ) = 0;

				std::thread					mTrimmingThread;

				std::condition_variable		mTrimmerWakeUp;

				/* TODO: The size parameter is now calculated during ThreadPool constuction. It might be valuable
				 * option to make it configurable somehow */
				std::atomic<unsigned int> 	mThreadPoolSize;

				std::atomic<unsigned int>	mWorkersToRemove;
			};

		private:

			class TrimOneAliveWorker
				:	public ITrimStrategy
			{
			public:
				TrimOneAliveWorker( ThreadPool * threadPool ) : ITrimStrategy( threadPool ) {}

			private:
				void trim( ThreadPool * threadPool ) override final
				{
	#if DEBUG_CONSOLE_OUTPUT
					std::cout << "Starting 'One alive worker' trimming strategy" << std::endl;
	#endif
				}
			};

			class TrimNAliveWorkers
				:	public ITrimStrategy
			{
			public:
				TrimNAliveWorkers( ThreadPool * threadPool ) : ITrimStrategy( threadPool ) {}

			private:
				void trim( ThreadPool * threadPool ) override final
				{
	#if DEBUG_CONSOLE_OUTPUT
					std::cout << "Starting 'N alive workers' trimming strategy" << std::endl;
	#endif
					while( true )
					{
						/* Lock the task queue to get thread safe access */
						std::unique_lock<std::mutex> workersListLock( threadPool->mWorkersMutex );

						unsigned int nActiveWorkers = 0U;

						for( const Core::ThreadPool::Worker & worker : threadPool->mWorkers )
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
								/* Construct in-place new worker at the end of the list */
								( threadPool->mWorkers ).emplace( ( threadPool->mWorkers ).end(), threadPool );
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
						( threadPool->mWorkers ).remove_if( WorkerRemovalPredicate() );

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
			};

		public:

			enum class Type : unsigned int
			{
				STRATEGY_1,
				STRATEGY_2
			};

			static std::unique_ptr<ITrimStrategy> create( const Type which, ThreadPool * threadPool )
			{
				switch( which )
				{
				default:
				case Type::STRATEGY_1 : return( std::make_unique<TrimOneAliveWorker>( threadPool ) ); break;
				case Type::STRATEGY_2 : return( std::make_unique<TrimNAliveWorkers>( threadPool ) ); break;
				}
			}

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
			:	/* Create the instance of trimming strategy */
				mTrimmer( TrimStrategy::create( TrimStrategy::Type::STRATEGY_2, this ) )
		{}

		~ThreadPool( void )
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

	public:

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
		 * @brief Task blocked notification
		 *
		 * This method shall be used inside task function to inform the thread pool the task is
		 * waiting for some event and thus the worker is blocked and not processing the task.
		 * If all the workers are theoretically in such state it might lead into thread pool deadlock.
		 * This mechanism is used to prevent that.
		 */
		void notifyBlockedTask( void )
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
		void notifyRunningTask( void )
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
		 * @brief Get the amount of tasks waiting in task queue
		 */
		std::size_t getNumOfTasksWaiting( void ) const
		{
			return( ( this->mTaskQueue ).size() );
		}

		/**
		 * @brief Get current worker being used
		 *
		 * This method gets the current thread ID and searches for a worker in which scope
		 * the method has been executed.
		 */
		Core::ThreadPool::Worker & getWorker( void )
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

		/**
		 * @brief Task type definition
		 *
		 * Task type to be used in task queue.
		 */
		using TTask = std::function<void( void )>;

	private:

		mutable std::mutex		mTaskQueueMutex;

		/* TODO: The type of queue might be changed to std::priority_queue. The tasks can then be a pack of
		 * the task itself and it's priority */
		std::queue<TTask>		mTaskQueue;

		/**
		 * @brief Workers
		 *
		 * ThreadPool workers container - extended threads which perform the jobs
		 * std::list is a container that supports constant time insertion and removal of elements
		 * from anywhere in the container. This feature is used in adding/removal of new workers.
		 */
		mutable std::mutex		mWorkersMutex;

		std::list<Worker>		mWorkers;

		std::condition_variable	mWorkerWakeUp;

		std::unique_ptr<TrimStrategy::ITrimStrategy>	mTrimmer;
	};
}
#endif /* THREADPOOL_THREADPOOL_H_ */
