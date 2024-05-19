#include "muduo/file/Uring.h"
#include "muduo/base/Logging.h"
#include "muduo/net/Channel.h"
#include "muduo/net/EventLoop.h"
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace muduo 
{
namespace file 
{

int createEventfd() 
{
  	int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  	if (evtfd < 0) 
	{
    	LOG_SYSERR << "Failed in eventfd";
    	abort();
  	}
  	return evtfd;
}

UringManager::UringManager(EventLoop *loop)
    : eventfd_(createEventfd()), channel_(loop, eventfd_) 
{
	currentIdx_ = 0;
	loop_ = loop;

	if (io_uring_queue_init(URING_ENTRYS, &uring_, 0) < 0) {
		LOG_SYSERR << "Failed to init iouring";
		abort();
	}
	if (io_uring_register_eventfd(&uring_, eventfd_) < 0) {
		LOG_SYSERR << "Failed to register eventfd on iouring";
		abort();
	}

	channel_.setReadCallback(std::bind(&UringManager::handleEventRead, this));
	channel_.enableReading();
}

UringManager::~UringManager()
{
	channel_.disableAll();
	channel_.remove();

	io_uring_unregister_eventfd(&uring_);
	io_uring_queue_exit(&uring_);

	::close(eventfd_);
}

void UringManager::handleEventRead()
{
	loop_->assertInLoopThread();
	uint64_t buf = 0;
	if (::read(eventfd_, &buf, 8) != 8) {
		LOG_SYSERR << "Failed to read from eventfd in iouring";
		abort();
	}

	handleCQE();
}

void UringManager::handleCQE()
{
	while (true)
	{
		io_uring_cqe *cqe = nullptr;
		int ret = io_uring_peek_cqe(&uring_, &cqe);
		if (ret == -EAGAIN) {
			LOG_DEBUG << "Consumed all pending CQEs";
			break;
		} else if (ret != 0) {
			LOG_SYSERR << "Failed to peek CQE";
		} else {
			assert(cqe != nullptr);
			size_t ioIdx = io_uring_cqe_get_data64(cqe);
			IOContext &ioCtx = activeIOs_.at(ioIdx);
			ioCtx.runCallBack(cqe->res);
			activeIOs_.erase(ioIdx);
			io_uring_cqe_seen(&uring_, cqe);

			/* handle one pending IO request */
			if (!pendingIOs_.empty()) 
			{
				IOContext pendIO = std::move(pendingIOs_.front());
				pendingIOs_.pop();
				appendIOContext(std::move(pendIO));
			}
		}
	}
}

void UringManager::appendIOContext(IOContext &&ctx)
{
	loop_->runInLoop([this, ctx{std::move(ctx)}](){
		io_uring_sqe *sqe = io_uring_get_sqe(&uring_);
		if (sqe == NULL) 
		{
			LOG_INFO << "No SQE available, add to pending IO list";
			pendingIOs_.emplace(std::move(ctx));
		}
		else 
		{
			int fd = ctx.file()->fd();
			const RWOperation& rwOp = ctx.rwOp();
			
			if (rwOp.dir() == RWOperation::READ) {
				io_uring_prep_readv(sqe, fd, rwOp.rawIov(), 
					static_cast<unsigned>(rwOp.iovSize()), static_cast<__u64>(rwOp.offset()));
			} else {
				io_uring_prep_writev(sqe, fd, rwOp.rawIov(), 
					static_cast<unsigned>(rwOp.iovSize()), static_cast<__u64>(rwOp.offset()));
			}

			size_t ioIdx = currentIdx_++;
			io_uring_sqe_set_data64(sqe, ioIdx);
			int ret = io_uring_submit(&uring_);
			assert(ret == 1);

			activeIOs_.emplace(ioIdx, std::move(ctx));
		}
	});
}

std::shared_ptr<File> UringManager::registerFile(const std::string& filePath)
{
	int fd = ::open(filePath.c_str(), O_RDWR);
	if (fd == -1)
	{
		LOG_WARN << "failed to open file " << filePath;
		return std::shared_ptr<File>();
	}
	return std::make_shared<File>(this, fd);
}

IOContext::IOContext(std::shared_ptr<File> &&file, RWOperation &&rwOp, RWCallBack &&callback)
	: file_(std::move(file)), rwOp_(std::move(rwOp)), callback_(std::move(callback))
{
}

void IOContext::runCallBack(int retval)
{
	callback_(retval, rwOp_);
}

File::~File()
{
	::close(fd_);
}

void File::asyncRW(RWOperation &&op, RWCallBack &&callBack)
{
	IOContext ctx(shared_from_this(), std::move(op), std::move(callBack));
	uring_->appendIOContext(std::move(ctx));
}

RWOperation::RWOperation(RWOperation::Dir dir, off_t fileOff, const iovec &defaultIOv)
{
	dir_ = dir;
	offset_ = fileOff;
	iovec_.emplace_back(defaultIOv);
}

void RWOperation::appendIOVec(const iovec &iov)
{
	iovec_.emplace_back(iov);
}

}
}