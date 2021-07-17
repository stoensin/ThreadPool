#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

// 线程池类
class ThreadPool {
  public:
	// 构造函数，传入线程数
	ThreadPool(size_t threads);
	// 入队任务(传入函数和函数的参数)
	template <class F, class... Args>
	auto enqueue(F &&f, Args &&... args)
		-> std::future<typename std::result_of<F(Args...)>::type>;
	// 一个最简单的函数包装模板可以这样写(C++11)适用于任何函数(变参、成员都可以)
	// template<class F, class... Args>
	// auto enqueue(F&& f, Args&&... args) -> decltype(declval<F>()(declval<Args>()...))
	// {    return f(args...); }
	// C++14 much easy:
	// template<class F, class... Args>
	// auto enqueue(F&& f, Args&&... args)
	// {    return f(args...); }

	// 析构函数
	~ThreadPool();

  private:
	// need to keep track of threads so we can join them
	// 工作线程组
	std::vector<std::thread> workers;
	// 任务队列
	std::queue<std::function<void()>> tasks;

	// synchronization 异步
	std::mutex queue_mutex;			   // 队列互斥锁
	std::condition_variable condition; // 条件变量
	bool stop;						   // 停止标志
};

// the constructor just launches some amount of workers
// 构造函数仅启动一些工作线程
inline ThreadPool::ThreadPool(size_t threads)
	: stop(false) {
	for (size_t i = 0; i < threads; ++i)
		// 添加线程到工作线程池
		workers.emplace_back( // 与push_back类型，但性能更好(与此类似的还有emplace/emlace_front)
			[this] {		  // 线程内不断的从任务队列取任务执行
				for (;;) {
					std::function<void()> task;

					{ //进入块级作用域，配合 拿锁unique_lock 表达式 当前作用域结束后 自动释放queue_mutex锁
						// 拿锁(独占所有权式)
						std::unique_lock<std::mutex> lock(this->queue_mutex);
						// 等待条件成立
						this->condition.wait(lock,
											 [this] { return this->stop || !this->tasks.empty(); });
						// wait此处线程开始进入休眠，直至被唤醒，会受到lambda表达式的结果影响
						// 执行条件变量等待wait的时候，已经拿到了锁(即lock已经拿到锁，没有阻塞)
						// 这里将会unlock释放锁，其他线程可以继续拿锁，但此处任然阻塞，等待条件成立
						// 一旦收到其他线程notify_*唤醒，则再次lock，然后进行条件判断
						// 当[return this->stop || !this->tasks.empty()]的结果为false将阻塞
						// 条件为true时候解除阻塞。此时lock依然为锁住状态
						// 由于刚开始创建线程池，线程池未停止，且任务队列为空，所以每个线程都会进入到wait状态。
                        // wait 运行机制如下： 
                        //                           void wait(unique_lock<mutex>& lock, _Predicate p)
                        //                           {
                        //                               while (!p())
                        //                                   wait(lock);
                        //                           }

						// 如果线程池停止或者任务队列为空，结束循环
						if (this->stop && this->tasks.empty()) {
							return;
						}
						// 此处已经满足 线程启池未停止 或者 任务队列已有任务
						// 取得任务队首任务
						task = std::move(this->tasks.front());
						// 从队列头移除任务
						this->tasks.pop();
					}
					// 执行任务
					task();
				}
			});
}


// 线程池中添加一个新的工作任务
template <class F, class... Args>
auto ThreadPool::enqueue(F &&f, Args &&... args)
	-> std::future<typename std::result_of<F(Args...)>::type> {
	//返回的类型auto推导出为放在std::future中的F(Args…) 返回类型的异步执行结果。
	
	using return_type = typename std::result_of<F(Args...)>::type;

	// 将任务函数和其参数绑定，构建一个packaged_task
	//创建一个智能指针task，其指向一个用std::bind(std::forward<F>(f), std::forward<Args>(args)... 来初始化的 std::packaged_task<return_type()> 对象例如:
  	//std::packaged_task<return_type()> t1(std::bind(std::forward<F>(f), std::forward<Args>(args)...)
  	//然后task指向了t1，即task指向了返回值为return_type的f(args)
	auto task = std::make_shared<std::packaged_task<return_type()>>(
		std::bind(std::forward<F>(f), std::forward<Args>(args)...));

	// 获取任务的future
	std::future<return_type> res = task->get_future();

	{
		// 独占拿锁
		std::unique_lock<std::mutex> lock(queue_mutex);

		// 不允许入队到已经停止的线程池
		if (stop) {
			throw std::runtime_error("enqueue on stopped ThreadPool");
		}
		// 将task所指向的f(args) 插入到tasks任务队列中。需要指出，这儿的emplace中传递的是构造函数的参数。
		//(*task)() ---> f(args)
		tasks.emplace([task]() { (*task)(); });
	}

	// 发送通知，唤醒线程池中某一个工作线程去执行任务
	condition.notify_one();
	return res;
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool() {
	{
		// 拿锁
		std::unique_lock<std::mutex> lock(queue_mutex);
		// 停止标志置true，防止新任务被加入到任务池
		stop = true;
	}
	// 通知所有工作线程，唤醒后因为stop为true了，所以都会结束
	condition.notify_all();
	// 等待所有工作线程结束
	for (std::thread &worker : workers) {
		worker.join();
	}
}

#endif
