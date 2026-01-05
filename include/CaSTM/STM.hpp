#pragma once 

#include "TransactionDescriptor.hpp"
#include "Transaction.hpp"
#include "TMVar.hpp"
#include "EBRManager/EBRManager.hpp"
#include <iostream>
#include <sys/types.h>
#include <thread>
#include <type_traits>



inline TransactionDescriptor& getLocalDescriptor () {
    static thread_local TransactionDescriptor desc;
    return desc;
}

inline Transaction& getLocalTransaction () {
    static thread_local Transaction tx(&getLocalDescriptor());
    return tx;
}

namespace STM {
    template<typename T>
    using Var = TMVar<T>;

    template<typename F>
    auto atomically(F&& func) {
        EBRManager::instance()->enter();
        Transaction& tx = getLocalTransaction();

        int retry_count = 0; // 计数器

        while (true) {
            try {
                tx.begin();
                if constexpr (std::is_void_v<std::invoke_result_t<F, Transaction&>>) {
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
            catch(const RetryException&){

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