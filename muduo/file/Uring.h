#ifndef MODUO_FILE_URING
#define MODUO_FILE_URING

#include "muduo/net/Channel.h"

#include <bits/types/struct_iovec.h>
#include <cstddef>
#include <functional>
#include <liburing.h>
#include <memory>
#include <queue>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace muduo 
{
namespace file 
{

using muduo::net::EventLoop;
using muduo::net::Channel;

class IOContext;
class UringManager;

// describe an IO operation on a particular File, corresponding to preadv/pwritev
class RWOperation
{
public:
    enum Dir { READ, WRITE };

    RWOperation(Dir dir, off_t fileOff, const iovec &defaultIOv);
    void appendIOVec(const iovec &iov);

    Dir dir() const { return dir_; }
    off_t offset() const { return offset_; }
    const iovec* rawIov() const { return iovec_.data(); }
    size_t iovSize() const { return iovec_.size(); }

private:
    Dir dir_;
    off_t offset_;
    std::vector<iovec> iovec_;
};

using RWCallBack = std::function<void(int, RWOperation&)>;

/*
User file handle, allow to register async IO operations.

Register a file path to UringManager to get a file object(shared_ptr),
file is closed when File object is destructed.
 */
class File: std::enable_shared_from_this<File>
{
public:

    File(UringManager *uring, int fd)
        : uring_(uring), fd_(fd) {}
    ~File();

    void asyncRW(RWOperation &&op, RWCallBack &&callBack);
    int fd() const { return fd_; }

private:
    UringManager *uring_;
    int fd_;
};

/*
Describe a registered and waiting for completion I/O in UringManager.
The argument of RWCallBack:
First int is the return value of the I/O operation;
Second RWOperation is the operation parameter used to register this IO.
*/
class IOContext
{
public:
    IOContext(std::shared_ptr<File> &&file, RWOperation &&rwOp, RWCallBack &&callback);
    void runCallBack(int retval);

    File* file() const { return file_.get(); }
    const RWOperation& rwOp() const { return rwOp_; }

private:
    std::shared_ptr<File> file_;
    RWOperation rwOp_;
    RWCallBack callback_;
};

/*
Async file read/write using io_uring and eventloop.
Not threadsafe, can only be used on the eventloop thread!
*/
class UringManager
{
public:
    UringManager(EventLoop *loop);
    ~UringManager();

    /*
    If succeed to open filePath, return a shared_ptr to File handle, ortherwise return null shared_ptr.

    Note: 
    Using shared_ptr to avoid unexpected destruction of File when async IOs still unfinish.
    The IOContext holds a shared_ptr to File.
    */
    std::shared_ptr<File> registerFile(const std::string& filePath);

private:

    void appendIOContext(IOContext &&ctx);
    void handleEventRead();
    void handleCQE();

    friend class File;

private:
    struct io_uring uring_;
    int eventfd_;
    Channel channel_;  // corresponds to eventfd_
    EventLoop *loop_;  // an io_uring instance binds to one eventloop(loop_)
    
    /*
    map<index, IOContext> for current IOs.
    index is used by io_uring's user_data to locate which IOContext in SQE and CQE.
    */
    std::unordered_map<size_t, IOContext> activeIOs_;
    size_t currentIdx_;

    /*
    For IO requests which SQE exhausted.
    Retry in handleCQE. 
    */
    std::queue<IOContext> pendingIOs_;

    static const unsigned URING_ENTRYS = 32;
};

}  // namespace file
}  // namespace muduo

#endif