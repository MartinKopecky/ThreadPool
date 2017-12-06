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
			Worker( ThreadPool * threadPool );

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
			~Worker( void );

			/**
			 * @brief Worker thread ID
			 *
			 * Returns the thread::id of the thread in which worker is executing it's tasks
			 */
			std::thread::id getID( void ) const;

			/**
			 * @brief Worker state evaluation
			 *
			 * @returns true	{Worker is currently in STATE given as template parameter}
			 * @returns false	{Worker state is different to STATE}
			 */
			template<typename STATE>
			bool inState( void ) const;

			/**
			 * @brief Worker is ready
			 *
			 * If the worker is either in IDLE or RUNNING state, it is considered active
			 */
			bool isActive( void ) const;

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
			bool isShutDown( void ) const;

			/* 'External' commands */

			/**
			 * @brief Set Worker to RUNNING state
			 *
			 * Thread safe method to set the worker state to RUNNING
			 */
			void run( void );

			/**
			 * @brief Set Worker to BLOCKED state
			 *
			 * Thread safe method to set the worker state to BLOCKED
			 */
			void blocked( void );

			/**
			 * @brief Terminate the worker
			 *
			 * Thread safe method to set the worker state to TERMINATING state
			 */
			void terminate( void );

			bool terminate_if( bool request );

		private:
		
			/**
			 * @brief Worker idle
			 *
			 * Thread safe method to set the worker state to IDLE state
			 */
			void idle( void );

			/**
			 * @brief Worker shutdown
			 *
			 * Thread safe method to set the worker state to SHUTDOWN state
			 */
			void shutdown( void );

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

				void run( Worker * worker ) override final;

				void terminate( Worker * worker ) override final;
			};

			class Running : public State
			{
			public:

				void blocked( Worker * worker ) override final;

				void idle( Worker * worker ) override final;

				void terminate( Worker * worker ) override final;
			};

			class Blocked : public State
			{
			public:

				void run( Worker * worker ) override final;
			};

			class Terminating : public State
			{
			public:

				void shutdown( Worker * worker ) override final;
			};

			class Shutdown : public State
			{};

			void taskRunner( void );

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

				ITrimStrategy( ThreadPool * threadPool );

				virtual ~ITrimStrategy( void );

				void setTrimTarget( unsigned int nWorkers );

				void notify( void );

				bool shouldTerminate( void ) const;

				void confirmTermination( void );

			protected:

				virtual void trim( ThreadPool * threadPool ) = 0;

				std::thread					mTrimmingThread;

				std::condition_variable		mTrimmerWakeUp;

				/* TODO: The size parameter is now calculated during ThreadPool constuction. It might be valuable
				 * option to make it configurable somehow */
				std::atomic<unsigned int> 	mThreadPoolSize;

				std::atomic<unsigned int>	mWorkersToRemove;
			};

		private:

			/**
			 * @brief Defensive trim strategy
			 *
			 * In defensive trim strategy one worker is always kept active. If all existing workers are being blocked,
			 * one new worker is added. Once any previously blocked worker gets running again, the amount of workers is
			 * trimmed again to required thread pool size.
			 */
			class DefensiveTrim
				:	public ITrimStrategy
			{
			public:

				DefensiveTrim( ThreadPool * threadPool ) : ITrimStrategy( threadPool ) {}

			private:

				void trim( ThreadPool * threadPool ) override final;
			};

			/**
			 * @brief Aggresive trim strategy
			 *
			 * In aggresive trim strategy defined (thread pool size) amount of workers is always kept active. If all
			 * existing workers are being blocked, this amount of new workers is added. Once any previously blocked worker
			 * gets running again, the amount of workers is trimmed again to required amount of active workers.
			 */
			class AggresiveTrim
				:	public ITrimStrategy
			{
			public:

				AggresiveTrim( ThreadPool * threadPool ) : ITrimStrategy( threadPool ) {}

			private:

				void trim( ThreadPool * threadPool ) override final;
			};

		public:

			enum class Type : unsigned int
			{
				DEFENSIVE,
				AGGRESIVE
			};

			static std::unique_ptr<ITrimStrategy> create( const Type which, ThreadPool * threadPool );

		};

#if THREADPOOL_IS_SINGLETON
	public:

		/**
		 * @brief ThreadPool instance management
		 */
		static ThreadPool & instance( void );

		/**
		 * @brief ThreadPool copy constructor [DELETED]
		 *
		 * ThreadPool is not copyable to respect singleton design pattern
		 */
		ThreadPool( ThreadPool const & ) = delete;

		/**
		 * @brief ThreadPool move constructor [DELETED]
		 *
		 * ThreadPool is not movable to respect singleton design pattern
		 */
		ThreadPool( ThreadPool && ) = delete;

		/**
		 * @brief ThreadPool copy assignment operator [DELETED]
		 *
		 * ThreadPool is not copy assignable to respect singleton design pattern
		 */
		ThreadPool & operator= ( ThreadPool const & ) = delete;

		/**
		 * @brief ThreadPool move assignment operator [DELETED]
		 *
		 * ThreadPool is not move assignable to respect singleton design pattern
		 */
		ThreadPool & operator= ( ThreadPool && ) = delete;

	protected:
#else
	public:
#endif

		/**
		 * @brief ThreadPool constructor
		 *
		 * This constructor must be made protected in case of ThreadPool being a singleton.
		 */
		ThreadPool( void );

		~ThreadPool( void );

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
		void notifyBlockedTask( void );

		/**
		 * @brief Task running notification
		 *
		 * This method shall be used inside task function to inform the thread pool the task is
		 * running again after some period of time spent in blocked mode.
		 */
		void notifyRunningTask( void );

	protected:

		/**
		 * @brief Get the amount of tasks waiting in task queue
		 */
		unsigned int getNumOfTasksWaiting( void ) const;

		/**
		 * @brief Get current worker being used
		 *
		 * This method gets the current thread ID and searches for a worker in which scope
		 * the method has been executed.
		 */
		Worker & getWorker( void );

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
