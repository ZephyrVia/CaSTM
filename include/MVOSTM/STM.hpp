#pragma once 

#include "MVOSTM/TransactionDescriptor.hpp"
#include "Transaction.hpp"
#include "TMVar.hpp"
#include "EBRManager/EBRManager.hpp"
#include <sys/types.h>
#include <thread>
#include <functional>
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