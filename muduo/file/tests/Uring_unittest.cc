#include "muduo/file/Uring.h"
#include "muduo/net/EventLoop.h"
#include <bits/types/struct_iovec.h>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#define FILE_NAME "test_file"
#define FILE_CONTENT "hello, uring!"
#define FILE_SIZE 13

using namespace muduo::net;
using namespace muduo::file;

void constructTestFileWithContent()
{
    std::ofstream file(FILE_NAME, std::ios::trunc);
    if (file)
    {
        file << FILE_CONTENT;
        file.flush();
        file.close();
    }
    else
    {
        abort();
    }
}

void constructTestFile()
{
    std::ofstream file(FILE_NAME, std::ios::trunc);
    if (file)
    {
        file.close();
    }
    else
    {
        abort();
    }
}

void removeTestFile()
{
    std::remove(FILE_NAME);
}

TEST(UringTest, Read)
{
    constructTestFileWithContent();

    EventLoop loop;
    char read_buf[FILE_SIZE] = {0};

    // 只能使用queueInLoop将其放在eventloop中执行，否则会直接执行
    loop.queueInLoop([&]() {
        auto uring_mgr = loop.getUringManager();
        auto file = uring_mgr->registerFile(FILE_NAME);
        file->asyncRW(RWOperation(RWOperation::READ, 0, 
            iovec{ .iov_base = read_buf, .iov_len = FILE_SIZE }), [&](int res, RWOperation& op){
            ASSERT_EQ(res, FILE_SIZE);
            loop.runInLoop([&loop](){
                loop.quit();
            });
        });
    });

    // 告诉loop需要执行上面的functor，否则一直陷入epoll
    loop.wakeup();
    loop.loop();

    ASSERT_EQ(std::string(read_buf, FILE_SIZE), std::string(FILE_CONTENT));

    removeTestFile();
}

TEST(UringTest, Write)
{
    constructTestFile();

    EventLoop loop;
    std::string write_str(FILE_CONTENT);

    loop.queueInLoop([&]() {
        auto uring_mgr = loop.getUringManager();
        auto file = uring_mgr->registerFile(FILE_NAME);
        file->asyncRW(RWOperation(RWOperation::WRITE, 0, 
        iovec{ .iov_base = write_str.data(), .iov_len = FILE_SIZE }), [&](int res, RWOperation& op){
            ASSERT_EQ(res, FILE_SIZE);
            loop.runInLoop([&loop](){
                loop.quit();
            });
        });
    });

    loop.wakeup();
    loop.loop();

    std::ifstream file(FILE_NAME);
    std::stringstream buf;
    buf << file.rdbuf();
    file.close();
    ASSERT_EQ(buf.str(), std::string(FILE_CONTENT));

    removeTestFile();
}

TEST(UringTest, MultiRead)
{
    constructTestFileWithContent();

    EventLoop loop;
    size_t count = 1;
    constexpr size_t read_count = 1024;
    
    for (size_t i = 0; i < read_count; ++i) {
        loop.queueInLoop([&]() {
            auto uring_mgr = loop.getUringManager();
            auto file = uring_mgr->registerFile(FILE_NAME);

            auto read_buf = std::make_shared<char[]>(FILE_SIZE);
            RWOperation rw_op(RWOperation::READ, 0, iovec{ .iov_base = read_buf.get(), .iov_len = FILE_SIZE });
            file->asyncRW(std::move(rw_op), 
            [&, read_buf](int res, RWOperation& op){
                ASSERT_EQ(res, FILE_SIZE);
                ASSERT_EQ(std::string(read_buf.get(), FILE_SIZE), std::string(FILE_CONTENT));
                if (count >= read_count)
                {
                    loop.runInLoop([&loop](){
                        loop.quit();
                    });
                }
                else
                    ++count;
            });
        });
    }

    loop.wakeup();
    loop.loop();

    removeTestFile();
}