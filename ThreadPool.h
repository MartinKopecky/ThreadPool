/*
 * ThreadPool.h
 *
 *  Created on: 4. 10. 2017
 *      Author: martin
 */

#ifndef THREADPOOL_THREADPOOL_H_
#define THREADPOOL_THREADPOOL_H_

/* TODO: Define that in compilation options */
#define THREADPOOL_IS_SINGLETON

#ifdef THREADPOOL_IS_SINGLETON
	#define USE_SINGLETON true
#else
	#define USE_SINGLETON false
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
#if USE_SINGLETON
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
#if USE_SINGLETON
		:	public boost::serialization::singleton<ThreadPool>
#endif
	{
	protected:
		class Worker
		{
		private:

			class WorkerState
			{
			public:
				/** @enum mapper::State
				 *  @brief Thread pool worker status
				 */
				enum class State: unsigned int
				{
					IDLE,		/**< No task is being executed */
					RUNNING,	/**< Worker is currently running a task */
					WAITING,	/**< Worker is waiting */
					TERMINATED,	/**< Worker is terminated */
				};

				WorkerState( void )
					:	mState( State::IDLE )
				{}

				void set( State state )
				{
					/* Request to terminate the worker should not be overwritten by some other state */
					if( this->mState != State::TERMINATED ) this->mState = state;

#if DEBUG_CONSOLE_OUTPUT
					switch( this->mState )
					{
						case State::IDLE : std::cout << "Worker is in IDLE state" << std::endl; break;
						case State::RUNNING : std::cout << "Worker is in RUNNING state" << std::endl; break;
						case State::WAITING : std::cout << "Worker is in WAITING state" << std::endl; break;
						case State::TERMINATED : std::cout << "Worker is in TERMINATED state" << std::endl; break;
						default : std::cout << "Worker is in UNKNOWN state" << std::endl; break;
					}
#endif /* DEBUG_CONSOLE_OUTPUT */
				}

				State get( void ) const { return( this->mState ); }

			private:
				std::atomic<State> mState;
			};

		public:

			/**
			 * @brief Thread pool worker constructor
			 *
			 * Runs the worker's taskRunner() in the stand alone thread.
			 */
			Worker( ThreadPool * threadPool )
				:	mWorkerState()
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
				/* Force termination if not yet done */
				if( ( this->mWorkerState ).get() != WorkerState::State::TERMINATED )
					( this->mWorkerState ).set( WorkerState::State::TERMINATED );

				/* Wait for the thread to finish */
				if( ( this->mThread ).joinable() ) ( this->mThread ).join();
			}

			void terminate( void ) { ( this->mWorkerState ).set( WorkerState::State::TERMINATED ); }

			/**
			 * @brief Task is waiting notification
			 *
			 * Notifies the worker the task which is currently executed is waiting for some event.
			 */
			void wait( void ) { ( this->mWorkerState ).set( WorkerState::State::WAITING ); }

			/**
			 * @brief Task is running notification
			 *
			 * Notifies the worker the task which is currently continuing its execution.
			 */
			void run( void ) { ( this->mWorkerState ).set( WorkerState::State::RUNNING ); }

			std::thread::id getID( void ) const
			{
				return( ( this->mThread ).get_id() );
			}

		private:

			/**
			 * @brief Worker task runner
			 *
			 * This method is the hearth of the worker. Internally it is an infinite loop able to be terminated
			 * by appropriate worker state (WorkerState::State::TERMINATED). Until not terminated, it continuously
			 * checks the task queue for tasks to be executed. If there are any, it fetches the task out of the queue
			 * and executes it.
			 * Once there are no tasks waiting in the queue, it goes into idle mode and waits there until new tasks
			 * are available or the worker is terminated.
			 */
			void taskRunner( ThreadPool * threadPool )
			{
				/* Infinite loop */
				while( ( this->mWorkerState ).get() != WorkerState::State::TERMINATED )
				{
					/* Lock the task queue to get thread safe access */
					std::unique_lock<std::mutex> taskQueueLock { threadPool->getTaskQueueMutex() };

					/* Check the amount of tasks waiting in the queue. If there aren't any, go to IDLE state
					 * and wait there till some tasks are available or the worker is terminated */
					if( threadPool->getNumOfTasksWaiting() == 0 )
					{
						(this->mWorkerState).set( WorkerState::State::IDLE );

						/* Wait for tasks to be available in the queue.
						 *
						 * The worker is blocked until the condition variable is notified by notify_one() or notify_all()
						 * OR
						 * the worker should be terminated.
						 */
						threadPool->mTasksAvailable.wait( taskQueueLock, [&]()
						{
							return( ( this->mWorkerState ).get() == WorkerState::State::TERMINATED );
						} );
					}

					/* Check for termination request again as it might be set during waiting for new tasks to come... */
					if( ( this->mWorkerState ).get() == WorkerState::State::TERMINATED ) break;

					/* It is about to execute the task so the worker is running again */
					( this->mWorkerState ).set( WorkerState::State::RUNNING );

					/* Fetch next task to be executed */
					TTask task = std::move( threadPool->fetchTask() );

					/* Unlock the task queue */
					taskQueueLock.unlock();

					/* Execute the task
					 * From within the task, the worker might be notified the task is waiting for some event using
					 * wait() and run() methods - handled via the ThreadPool instance. So the worker state might be
					 * switched between WorkerState::State::RUNNING and WorkerState::State::WAITING */
					task();
				}
			}

			WorkerState	mWorkerState;

			std::thread	mThread;
		};

#if USE_SINGLETON
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
		{
			for( unsigned int i = 0; i < ( std::thread::hardware_concurrency() - 1 ); i++ )
			{
				( this->mWorkers ).emplace( ( this->mWorkers ).end(), std::make_shared<Worker>( this ) );
			}
		}

		~ThreadPool( void )
		{
			/* First of all, all workers must be stopped */
			for( auto & worker : this->mWorkers )
			{
				worker->terminate();
			}
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

			// get the future to return later
			std::future<typename std::result_of<FUNCTION( ARGUMENTS... )>::type> ReturnFuture = tTask->get_future();

			/* Thread-safe task queue access block */
			{
				/* Lock task queue */
				std::lock_guard<std::mutex> lock( this->getTaskQueueMutex() );

				this->mTaskQueue.emplace( [tTask]() { (* tTask)(); } );

				/* The lock guard expires here so the task queue gets unlocked */
			}

			// let a waiting thread know there is an available job
			this->mTasksAvailable.notify_one();

			return ReturnFuture;
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
			/* Check wheter there is matching worker and is still valid */
			if( !( this->getWorker() ).expired() )
			{
				/* Call worker wait() method */
				( ( this->getWorker() ).lock() )->wait();
			}
		}

		/**
		 * @brief Worker run notification
		 *
		 * This method shall be used inside task function to inform the thread pool the task is
		 * running again after some period of time spent in waiting mode.
		 */
		void run( void )
		{
			/* Check wheter there is matching worker and is still valid */
			if( !( this->getWorker() ).expired() )
			{
				/* Call worker run() method */
				( ( this->getWorker() ).lock() )->run();
			}
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

		/**
		 * @brief Fetch the task from task queue
		 *
		 * Fetches and removes the task from the task queue and returns it for further processing.
		 */
		TTask fetchTask( void )
		{
			TTask task;

			/* Check whether the task queue is empty. */
			if( !( this->mTaskQueue ).empty() )
			{
				task = ( this->mTaskQueue ).front();

				( this->mTaskQueue ).pop();
			}

			return( std::move( task ) );
		}

		/**
		 * @brief Get task queue mutex
		 *
		 * Returns a reference to mutex securing the task queue in terms of concurrent access.
		 */
		std::mutex & getTaskQueueMutex( void ) const
		{
			return( this->mTaskQueueMutex );
		}

	private:

		mutable std::mutex					mTaskQueueMutex;

		std::queue<TTask>					mTaskQueue;

		std::condition_variable				mTasksAvailable;

		/**
		 * @brief Workers
		 *
		 * ThreadPool workers container - extended threads which perform the jobs
		 * std::list is a container that supports constant time insertion and removal of elements
		 * from anywhere in the container. This feature is used in adding/removal of new workers.
		 */
		std::list<std::shared_ptr<Worker>>	mWorkers;
	};
}
#endif /* THREADPOOL_THREADPOOL_H_ */
