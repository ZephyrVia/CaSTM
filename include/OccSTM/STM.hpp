#pragma once 

#include "TransactionDescriptor.hpp"
#include "Transaction.hpp"
#include "TMVar.hpp"
#include "EBRManager/EBRManager.hpp"
#include <iostream>
#include <sys/types.h>
#include <thread>
#include <type_traits>

namespace STM {

    // 将具体的实现细节（全局获取函数）移入 Occ 命名空间
    namespace Occ {

        // 假设 TransactionDescriptor 已经在 STM::Occ 中定义
        inline TransactionDescriptor& getLocalDescriptor () {
            static thread_local TransactionDescriptor desc;
            return desc;
        }

        // 假设 Transaction 已经在 STM::Occ 中定义
        inline Transaction& getLocalTransaction () {
            static thread_local Transaction tx(&getLocalDescriptor());
            return tx;
        }
    }

    // ================= 用户接口层 =================

    // 对外暴露的 Var，指向 Occ 实现
    template<typename T>
    using Var = Occ::TMVar<T>;

    template<typename F>
    auto atomically(F&& func) {
        EBRManager::instance()->enter();
        
        // 【关键修改】调用 Occ 命名空间下的函数
        Occ::Transaction& tx = Occ::getLocalTransaction();

        int retry_count = 0; // 计数器

        while (true) {
            try {
                tx.begin();

                // 【关键修改】类型推导增加 Occ:: 前缀
                if constexpr (std::is_void_v<std::invoke_result_t<F, Occ::Transaction&>>) {
                    func(tx);

                    if(tx.commit()) {
                        break;
                    }
                } 
                else {
                    auto result = func(tx);
                    if(tx.commit()) {
                        EBRManager::instance()->leave();
                        return result;
                    }
                }
            } 
            // 【关键修改】捕获 Occ::RetryException
            catch(const Occ::RetryException&){

                retry_count++;
                // 每重试 1000 次打印一下，防止刷屏
                if (retry_count % 1000 == 0) {
                    std::cout << "[Thread " << std::this_thread::get_id() 
                            << "] Retrying... Count: " << retry_count << std::endl;
                }
                std::this_thread::yield();
                continue;
            }
            catch(...) {
                EBRManager::instance()->leave();
                throw;
            }
        }

        EBRManager::instance()->leave();
    }
}