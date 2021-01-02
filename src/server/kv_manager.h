/* Copyright 2020-present VidarDB Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef KV_MANAGER_H_
#define KV_MANAGER_H_


#include <unordered_map>
using namespace std;

#include "ipc/kv_mq.h"
#include "kv_worker.h"


extern void LaunchKVManager();

/*
 * A kv manager is responsible for managing all the kv workers' lifecycle from
 * multiple databases, so will only be terminated when PostgreSQL is stop.
 */

class KVManager {
  public:
    KVManager();
    ~KVManager();

    void Start();
    void Run();
    void Stop();

  private:
    void Launch(KVWorkerId workerId, const KVMessage& msg);
    void Terminate(KVWorkerId workerId, const KVMessage& msg);
    bool CheckKVWorkerAlive(BackgroundWorkerHandle* handle);
    void TerminateKVWorker(BackgroundWorkerHandle* worker);

    unordered_map<KVWorkerId, KVWorkerHandle*> workers_;
    KVMessageQueue* queue_;
    bool running_;
};

/*
 * A kv manager client which as a stub to interact with kv manager process
 * through its corresponding message queue.
 */

class KVManagerClient {
  public:
    KVManagerClient();
    ~KVManagerClient();

    bool Launch(KVWorkerId workerId);
    bool Terminate(KVWorkerId workerId, KVDatabaseId dbId);
    void Notify(KVCtrlType type);

  private:
    KVMessageQueue* queue_;
};

#endif  /* KV_MANAGER_H_ */
