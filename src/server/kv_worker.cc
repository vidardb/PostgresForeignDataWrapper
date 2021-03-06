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

#include "kv_worker.h"
#include "kv_manager.h"
#include "ipc/kv_posix.h"
#include "kv_storage.h"
#include "fcntl.h"

extern "C" {
#include "postgres.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
}


#define READBATCHPATH     "/KVReadBatch"
#ifdef VIDARDB
#define RANGEQUERYPATH    "/KVRangeQuery"
#endif

static const char* WORKER = "Worker";


/*
 * Implementation for kv worker
 */

KVWorker::KVWorker(KVWorkerId workerId, KVDatabaseId dbId) {
    running_ = false;
    conn_ = nullptr;
    ref_ = 0;
    queue_ = new KVMessageQueue(workerId, WORKER, true);
}

KVWorker::~KVWorker() {
    if (conn_) {
        CloseConn(conn_);
    }
    delete queue_;
}

void KVWorker::Start() {
    running_ = true;
}

void KVWorker::Run() {
    while (running_) {
        KVMessage msg;
        queue_->Recv(msg, MSGHEADER);

        switch (msg.hdr.op) {
            case KVOpDummy:
                break;
            case KVOpOpen:
                Open(msg);
                break;
            case KVOpClose:
                Close(msg);
                break;
            case KVOpCount:
                Count(msg);
                break;
            case KVOpPut:
                Put(msg);
                break;
            case KVOpGet:
                Get(msg);
                break;
            case KVOpDel:
                Delete(msg);
                break;
            case KVOpLoad:
                Load(msg);
                break;
            case KVOpReadBatch:
                ReadBatch(msg);
                break;
            case KVOpDelCursor:
                CloseCursor(msg);
                break;
            #ifdef VIDARDB
            case KVOpRangeQuery:
                RangeQuery(msg);
                break;
            case KVOpClearRangeQuery:
                ClearRangeQuery(msg);
                break;
            #endif
            case KVOpTerminate:
                Terminate(msg);
                break;
            default:
                ereport(WARNING, errmsg("invalid operation: %d", msg.hdr.op));
        }
    }
}

void KVWorker::Stop() {
    running_ = false;
    queue_->Stop();
}

void KVWorker::ReadOpenArgs(KVChannel* channel, uint64* offset, void* entity,
                            uint64 size) {
    OpenArgs* args = static_cast<OpenArgs*>(entity);
    uint64 delta = sizeof(args->opts);

    channel->Pop(offset, reinterpret_cast<char*>(&args->opts), delta);
    #ifdef VIDARDB
    uint64 len = sizeof(args->useColumn);
    channel->Pop(offset, reinterpret_cast<char*>(&args->useColumn), len);
    delta += len;
    len = sizeof(args->attrCount);
    channel->Pop(offset, reinterpret_cast<char*>(&args->attrCount), len);
    delta += len;
    #endif
    channel->Pop(offset, args->path, size - delta);
}

void KVWorker::Open(KVMessage& msg) {
    OpenArgs args;
    args.path = static_cast<char*>(palloc0(msg.hdr.etySize));

    msg.ety = &args;
    msg.readFunc = ReadOpenArgs;
    queue_->Recv(msg, MSGENTITY);

    if (!conn_) {
        #ifdef VIDARDB
        conn_ = OpenConn(args.path, args.useColumn, args.attrCount, &args.opts);
        #else
        conn_ = OpenConn(args.path, &args.opts);
        #endif
    }
    ref_++;

    pfree(args.path);
}

void KVWorker::Close(KVMessage& msg) {
    queue_->Recv(msg, MSGDISCARD);

    if (conn_) {
        ref_--;
    }
}

void KVWorker::Count(KVMessage& msg) {
    queue_->Recv(msg, MSGDISCARD);

    uint64 count = GetCount(conn_);

    KVMessage sendmsg;
    sendmsg.ety = &count;
    sendmsg.hdr.etySize = sizeof(count);
    sendmsg.hdr.rpsId = msg.hdr.rpsId;
    sendmsg.writeFunc = CommonWriteEntity;

    queue_->Send(sendmsg);
}

void KVWorker::Put(KVMessage& msg) {
    msg.ety = palloc0(msg.hdr.etySize);
    msg.readFunc = CommonReadEntity;
    queue_->Recv(msg, MSGENTITY);

    PutArgs args;
    args.keyLen = *static_cast<uint64*>(msg.ety);
    args.valLen = msg.hdr.etySize - args.keyLen - sizeof(args.keyLen);
    args.key = static_cast<char*>(msg.ety) + sizeof(args.keyLen);
    args.val = static_cast<char*>(msg.ety) + sizeof(args.keyLen) + args.keyLen;

    bool success = PutRecord(conn_, args.key, args.keyLen, args.val, args.valLen);
    queue_->Send(success ? SuccessMessage(msg.hdr.rpsId) :
                           FailureMessage(msg.hdr.rpsId));

    pfree(msg.ety);
}

void KVWorker::Get(KVMessage& msg) {
    msg.ety = palloc0(msg.hdr.etySize);
    msg.readFunc = CommonReadEntity;
    queue_->Recv(msg, MSGENTITY);

    char*  val = nullptr;
    uint64 valLen;
    bool success = GetRecord(conn_, static_cast<char*>(msg.ety), msg.hdr.etySize,
                             &val, &valLen);
    if (success) {
        KVMessage sendmsg = SuccessMessage(msg.hdr.rpsId);
        sendmsg.hdr.etySize = valLen;
        sendmsg.ety = val;
        sendmsg.writeFunc = CommonWriteEntity;
        queue_->Send(sendmsg);
        pfree(val);
    } else {
        queue_->Send(FailureMessage(msg.hdr.rpsId));
    }

    pfree(msg.ety);
}

void KVWorker::Delete(KVMessage& msg) {
    msg.ety = palloc0(msg.hdr.etySize);
    msg.readFunc = CommonReadEntity;
    queue_->Recv(msg, MSGENTITY);

    bool success = DelRecord(conn_, static_cast<char*>(msg.ety), msg.hdr.etySize);
    queue_->Send(success ? SuccessMessage(msg.hdr.rpsId) :
                           FailureMessage(msg.hdr.rpsId));

    pfree(msg.ety);
}

void KVWorker::Load(KVMessage& msg) {
    msg.ety = palloc0(msg.hdr.etySize);
    msg.readFunc = CommonReadEntity;
    queue_->Recv(msg, MSGENTITY);

    PutArgs args;
    args.keyLen = *static_cast<uint64*>(msg.ety);
    args.valLen = msg.hdr.etySize - args.keyLen - sizeof(args.keyLen);
    args.key = static_cast<char*>(msg.ety) + sizeof(args.keyLen);
    args.val = static_cast<char*>(msg.ety) + sizeof(args.keyLen) + args.keyLen;

    PutRecord(conn_, args.key, args.keyLen, args.val, args.valLen);

    pfree(msg.ety);
}

void KVWorker::WriteReadBatchState(KVChannel* channel, uint64* offset,
                                   void* entity, uint64 size) {
    ReadBatchState* state = static_cast<ReadBatchState*>(entity);

    channel->Push(offset, reinterpret_cast<char*>(&state->next),
                  sizeof(state->next));
    channel->Push(offset, reinterpret_cast<char*>(&state->size),
                  sizeof(state->size));
    /* why this does not work? */
//    channel->Push(offset, reinterpret_cast<char*>(state), sizeof(*state));
}

void KVWorker::ReadBatch(KVMessage& msg) {
    msg.ety = palloc0(msg.hdr.etySize);
    msg.readFunc = CommonReadEntity;
    queue_->Recv(msg, MSGENTITY);

    KVCursorKey key;
    key.pid = *static_cast<pid_t*>(msg.ety);
    key.opid =
        *reinterpret_cast<KVOpId*>(static_cast<char*>(msg.ety) + sizeof(key.pid));

    void* cursor = nullptr;
    auto it = cursors_.find(key);
    if (it == cursors_.end()) {
        cursor = GetIter(conn_);
        cursors_.insert({key, cursor});
    } else {
        cursor = it->second;
    }

    char name[MAXPATHLENGTH];
    snprintf(name, MAXPATHLENGTH, "%s%d%d%lu", READBATCHPATH, key.pid,
             msg.hdr.relId, key.opid);
    ShmUnlink(name, __func__);
    int fd = ShmOpen(name, O_CREAT | O_RDWR, 0777, __func__);
    Ftruncate(fd, READBATCHSIZE, __func__);
    char* shm = (char*) Mmap(nullptr, READBATCHSIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0, __func__);
    Fclose(fd, __func__);

    ReadBatchState state;
    state.next = BatchRead(conn_, cursor, shm, &state.size);

    Munmap(shm, READBATCHSIZE, __func__); /* Is it safe to unmap here? */

    KVMessage sendmsg = SuccessMessage(msg.hdr.rpsId);
    sendmsg.hdr.etySize = sizeof(state);
    sendmsg.ety = &state;
    sendmsg.writeFunc = WriteReadBatchState;

    queue_->Send(sendmsg);

    pfree(msg.ety);
}

void KVWorker::CloseCursor(KVMessage& msg) {
    msg.ety = palloc0(msg.hdr.etySize);
    msg.readFunc = CommonReadEntity;
    queue_->Recv(msg, MSGENTITY);

    KVCursorKey key;
    key.pid = *static_cast<pid_t*>(msg.ety);
    key.opid =
        *reinterpret_cast<KVOpId*>(static_cast<char*>(msg.ety) + sizeof(key.pid));

    auto it = cursors_.find(key);
    if (it == cursors_.end()) {
        pfree(msg.ety);
        return;
    }

    DelIter(it->second);
    cursors_.erase(it);
    pfree(msg.ety);
}

#ifdef VIDARDB
void KVWorker::RangeQuery(KVMessage& msg) {
    msg.ety = palloc0(msg.hdr.etySize);
    msg.readFunc = CommonReadEntity;
    queue_->Recv(msg, MSGENTITY);

    KVCursorKey key;
    char* current = static_cast<char*>(msg.ety);
    key.pid = *reinterpret_cast<pid_t*>(current);
    current += sizeof(key.pid);
    key.opid = *reinterpret_cast<KVOpId*>(current);
    current += sizeof(key.opid);

    KVRangeQueryEntry entry;
    auto it = ranges_.find(key);
    if (it == ranges_.end()) {
        RangeQueryOpts opts;

        opts.startLen = *reinterpret_cast<uint64*>(current);
        current += sizeof(opts.startLen);

        if (opts.startLen > 0) {
            opts.start = static_cast<char*>(palloc0(opts.startLen));
            memcpy(opts.start, current, opts.startLen);
            current += opts.startLen;
        }

        opts.limitLen = *reinterpret_cast<uint64*>(current);
        current += sizeof(opts.limitLen);

        if (opts.limitLen > 0) {
            opts.limit = static_cast<char*>(palloc0(opts.limitLen));
            memcpy(opts.limit, current, opts.limitLen);
            current += opts.limitLen;
        }

        opts.batchCapacity = *reinterpret_cast<uint64*>(current);
        current += sizeof(opts.batchCapacity);

        opts.attrCount = *reinterpret_cast<int*>(current);
        current += sizeof(opts.attrCount);

        if (opts.attrCount > 0) {
            opts.attrs = reinterpret_cast<AttrNumber*>(current);
        }

        ParseRangeQueryOptions(&opts, &entry.range, &entry.readOpts);
        ranges_.insert({key, entry});
    } else {
        entry = it->second;
    }

    void* result = nullptr;
    ReadBatchState state;

    do {
        state.next = RangeQueryRead(conn_, entry.range, &entry.readOpts,
                                    &state.size, &result);
    } while (state.next && state.size == 0);

    char name[MAXPATHLENGTH];
    snprintf(name, MAXPATHLENGTH, "%s%d%d%lu", RANGEQUERYPATH, key.pid,
             msg.hdr.relId, key.opid);
    ShmUnlink(name, __func__);

    char* shm = nullptr;
    if (state.size > 0) {
        int fd = ShmOpen(name, O_CREAT | O_RDWR, 0777, __func__);
        Ftruncate(fd, state.size, __func__);
        shm = (char*) Mmap(nullptr, state.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                           fd, 0, __func__);
        Fclose(fd, __func__);
    }

    ParseRangeQueryResult(result, shm);
    if (state.size > 0) {
        Munmap(shm, state.size, __func__);
    }

    KVMessage sendmsg = SuccessMessage(msg.hdr.rpsId);
    sendmsg.hdr.etySize = sizeof(state);
    sendmsg.ety = &state;
    sendmsg.writeFunc = WriteReadBatchState;

    queue_->Send(sendmsg);

    pfree(msg.ety);
}

void KVWorker::ClearRangeQuery(KVMessage& msg) {
    msg.ety = palloc0(msg.hdr.etySize);
    msg.readFunc = CommonReadEntity;
    queue_->Recv(msg, MSGENTITY);

    KVCursorKey key;
    key.pid = *static_cast<pid_t*>(msg.ety);
    key.opid =
        *reinterpret_cast<KVOpId*>(static_cast<char*>(msg.ety) + sizeof(key.pid));

    auto it = ranges_.find(key);
    if (it == ranges_.end()) {
        pfree(msg.ety);
        return;
    }

    ClearRangeQueryMeta(it->second.range, it->second.readOpts);
    ranges_.erase(it);

    char name[MAXPATHLENGTH];
    snprintf(name, MAXPATHLENGTH, "%s%d%d%lu", RANGEQUERYPATH, key.pid,
             msg.hdr.relId, key.opid);
    ShmUnlink(name, __func__);
    pfree(msg.ety);
}
#endif

void KVWorker::Terminate(KVMessage& msg) {
    queue_->Recv(msg, MSGDISCARD);

    Stop();
}


/*
 * Implementation for kv worker client
 */

KVWorkerClient::KVWorkerClient(KVWorkerId workerId) {
    queue_ = new KVMessageQueue(workerId, WORKER, false);
}

KVWorkerClient::~KVWorkerClient() {
    delete queue_;
}

void KVWorkerClient::WriteOpenArgs(KVChannel* channel, uint64* offset,
                                   void* entity, uint64 size) {
    OpenArgs* args = static_cast<OpenArgs*>(entity);

    channel->Push(offset, reinterpret_cast<char*>(&args->opts),
                  sizeof(args->opts));
    #ifdef VIDARDB
    channel->Push(offset, reinterpret_cast<char*>(&args->useColumn),
                  sizeof(args->useColumn));
    channel->Push(offset, reinterpret_cast<char*>(&args->attrCount),
                  sizeof(args->attrCount));
    #endif
    channel->Push(offset, args->path, strlen(args->path));
}

void KVWorkerClient::Open(KVWorkerId workerId, OpenArgs* args) {
    uint64 size = sizeof(args->opts) + strlen(args->path);
    #ifdef VIDARDB
    size += sizeof(args->useColumn) + sizeof(args->attrCount);
    #endif

    KVMessage sendmsg = SimpleMessage(KVOpOpen, workerId, MyDatabaseId);
    sendmsg.ety = args;
    sendmsg.hdr.etySize = size;
    sendmsg.writeFunc = WriteOpenArgs;

    queue_->Send(sendmsg);
}

void KVWorkerClient::Close(KVWorkerId workerId) {
    queue_->Send(SimpleMessage(KVOpClose, workerId, MyDatabaseId));
}

uint64 KVWorkerClient::Count(KVWorkerId workerId) {
    uint64 count;

    KVMessage recvmsg;
    recvmsg.ety = &count;
    recvmsg.hdr.etySize = sizeof(count);
    recvmsg.readFunc = CommonReadEntity;

    KVMessage sendmsg = SimpleMessage(KVOpCount, workerId, MyDatabaseId);
    queue_->SendWithResponse(sendmsg, recvmsg);

    return count;
}

void KVWorkerClient::WritePutArgs(KVChannel* channel, uint64* offset,
                                  void* entity, uint64 size) {
    PutArgs* args = static_cast<PutArgs*>(entity);

    channel->Push(offset, reinterpret_cast<char*>(&args->keyLen),
                  sizeof(args->keyLen));
    channel->Push(offset, args->key, args->keyLen);
    channel->Push(offset, args->val, args->valLen);
}

bool KVWorkerClient::Put(KVWorkerId workerId, PutArgs* args) {
    uint64 size = args->keyLen + args->valLen + sizeof(args->keyLen);

    KVMessage sendmsg = SimpleMessage(KVOpPut, workerId, MyDatabaseId);
    sendmsg.ety = args;
    sendmsg.hdr.etySize = size;
    sendmsg.writeFunc = WritePutArgs;

    KVMessage recvmsg;
    queue_->SendWithResponse(sendmsg, recvmsg);

    return recvmsg.hdr.status == KVStatusSuccess;
}

bool KVWorkerClient::Get(KVWorkerId workerId, GetArgs* args) {
    KVMessage sendmsg = SimpleMessage(KVOpGet, workerId, MyDatabaseId);
    sendmsg.ety = args->key;
    sendmsg.hdr.etySize = args->keyLen;
    sendmsg.writeFunc = CommonWriteEntity;

    KVMessage recvmsg;
    uint32 channel = queue_->LeaseResponseChannel();
    sendmsg.hdr.rpsId = channel;
    recvmsg.hdr.rpsId = channel;
    queue_->Send(sendmsg);
    queue_->Recv(recvmsg, MSGHEADER);

    *(args->valLen) = recvmsg.hdr.etySize;
    *(args->val) = static_cast<char*>(palloc0(*(args->valLen)));
    recvmsg.ety = *(args->val);
    recvmsg.readFunc = CommonReadEntity;
    queue_->Recv(recvmsg, MSGENTITY);
    queue_->UnleaseResponseChannel(channel);

    return recvmsg.hdr.status == KVStatusSuccess;
}

bool KVWorkerClient::Delete(KVWorkerId workerId, DeleteArgs* args) {
    KVMessage sendmsg = SimpleMessage(KVOpDel, workerId, MyDatabaseId);
    sendmsg.ety = args->key;
    sendmsg.hdr.etySize = args->keyLen;
    sendmsg.writeFunc = CommonWriteEntity;

    KVMessage recvmsg;
    queue_->SendWithResponse(sendmsg, recvmsg);

    return recvmsg.hdr.status == KVStatusSuccess;
}

void KVWorkerClient::Load(KVWorkerId workerId, PutArgs* args) {
    uint64 size = args->keyLen + args->valLen + sizeof(args->keyLen);

    KVMessage sendmsg = SimpleMessage(KVOpLoad, workerId, MyDatabaseId);
    sendmsg.ety = args;
    sendmsg.hdr.etySize = size;
    sendmsg.writeFunc = WritePutArgs;

    queue_->Send(sendmsg);
}

void KVWorkerClient::WriteReadBatchArgs(KVChannel* channel, uint64* offset,
                                        void* entity, uint64 size) {
    ReadBatchArgs* args = static_cast<ReadBatchArgs*>(entity);

    pid_t pid = getpid();
    channel->Push(offset, reinterpret_cast<char*>(&pid), sizeof(pid_t));
    channel->Push(offset, reinterpret_cast<char*>(&args->opid), sizeof(args->opid));
}

bool KVWorkerClient::ReadBatch(KVWorkerId workerId, ReadBatchArgs* args) {
    if (*(args->buf) != nullptr) {
        Munmap(*(args->buf), READBATCHSIZE, __func__);
    }

    KVMessage sendmsg = SimpleMessage(KVOpReadBatch, workerId, MyDatabaseId);
    sendmsg.ety = args;
    sendmsg.hdr.etySize = sizeof(pid_t) + sizeof(args->opid);
    sendmsg.writeFunc = WriteReadBatchArgs;

    char buf[sizeof(bool) + sizeof(uint64)];
    KVMessage recvmsg;
    recvmsg.ety = buf;
    recvmsg.readFunc = CommonReadEntity;
    queue_->SendWithResponse(sendmsg, recvmsg);

    if (recvmsg.hdr.status != KVStatusSuccess) {
        return false;
    }

    bool next = *reinterpret_cast<bool*>(buf);
    *(args->bufLen) = *reinterpret_cast<uint64*>(buf + sizeof(next));
    if (*(args->bufLen) == 0) {
        *(args->buf) = nullptr;
    } else {
        char  name[MAXPATHLENGTH];
        pid_t pid = getpid();

        snprintf(name, MAXPATHLENGTH, "%s%d%d%lu", READBATCHPATH, pid, workerId,
                 args->opid);
        int fd = ShmOpen(name, O_RDWR, 0777, __func__);
        *(args->buf) = (char*) Mmap(nullptr, READBATCHSIZE, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0, __func__);
        Fclose(fd, __func__);
    }

    return next;
}

void KVWorkerClient::WriteDelCursorArgs(KVChannel* channel, uint64* offset,
                                        void* entity, uint64 size) {
    CloseCursorArgs* args = static_cast<CloseCursorArgs*>(entity);

    pid_t pid = getpid();
    channel->Push(offset, reinterpret_cast<char*>(&pid), sizeof(pid_t));
    channel->Push(offset, reinterpret_cast<char*>(&args->opid),
                  sizeof(args->opid));
}

void KVWorkerClient::CloseCursor(KVWorkerId workerId, CloseCursorArgs* args) {
    if (args->buf) {
        Munmap(args->buf, READBATCHSIZE, __func__);
    }

    char  name[MAXPATHLENGTH];
    pid_t pid = getpid();

    snprintf(name, MAXPATHLENGTH, "%s%d%d%lu", READBATCHPATH, pid, workerId,
             args->opid);
    ShmUnlink(name, __func__);

    KVMessage sendmsg = SimpleMessage(KVOpDelCursor, workerId, MyDatabaseId);
    sendmsg.ety = args;
    sendmsg.hdr.etySize = sizeof(pid_t) + sizeof(args->opid);
    sendmsg.writeFunc = WriteDelCursorArgs;

    queue_->Send(sendmsg);
}

#ifdef VIDARDB
void KVWorkerClient::WriteRangeQueryArgs(KVChannel* channel, uint64* offset,
                                         void* entity, uint64 size) {
    RangeQueryArgs* args = static_cast<RangeQueryArgs*>(entity);
    RangeQueryOpts* opts = args->opts;

    pid_t pid = getpid();
    channel->Push(offset, reinterpret_cast<char*>(&pid), sizeof(pid_t));
    channel->Push(offset, reinterpret_cast<char*>(&args->opid), sizeof(args->opid));

    if (opts) {
        channel->Push(offset, reinterpret_cast<char*>(&opts->startLen),
                      sizeof(opts->startLen));
        if (opts->startLen > 0) {
            channel->Push(offset, opts->start, opts->startLen);
        }

        channel->Push(offset, reinterpret_cast<char*>(&opts->limitLen),
                      sizeof(opts->limitLen));
        if (opts->limitLen > 0) {
            channel->Push(offset, opts->limit, opts->limitLen);
        }

        channel->Push(offset, reinterpret_cast<char*>(&opts->batchCapacity),
                      sizeof(opts->batchCapacity));
        channel->Push(offset, reinterpret_cast<char*>(&opts->attrCount),
                      sizeof(opts->attrCount));
        if (opts->attrCount > 0) {
            channel->Push(offset, reinterpret_cast<char*>(opts->attrs),
                          opts->attrCount * sizeof(*(opts->attrs)));
        }
    }
}

bool KVWorkerClient::RangeQuery(KVWorkerId workerId, RangeQueryArgs* args) {
    if (*(args->buf) && *(args->bufLen) > 0) {
        Munmap(*(args->buf), *(args->bufLen), __func__);
    }

    KVMessage sendmsg = SimpleMessage(KVOpRangeQuery, workerId, MyDatabaseId);
    sendmsg.ety = args;
    sendmsg.hdr.etySize = sizeof(pid_t) + sizeof(args->opid);
    if (args->opts) {
        sendmsg.hdr.etySize += sizeof(args->opts->startLen);
        sendmsg.hdr.etySize += args->opts->startLen;
        sendmsg.hdr.etySize += sizeof(args->opts->limitLen);
        sendmsg.hdr.etySize += args->opts->limitLen;
        sendmsg.hdr.etySize += sizeof(args->opts->attrCount);
        sendmsg.hdr.etySize += args->opts->attrCount * sizeof(*(args->opts->attrs));
        sendmsg.hdr.etySize += sizeof(args->opts->batchCapacity);
    }
    sendmsg.writeFunc = WriteRangeQueryArgs;

    char buf[sizeof(bool) + sizeof(uint64)];
    KVMessage recvmsg;
    recvmsg.ety = buf;
    recvmsg.readFunc = CommonReadEntity;
    queue_->SendWithResponse(sendmsg, recvmsg);

    if (recvmsg.hdr.status != KVStatusSuccess) {
        return false;
    }

    bool next = *reinterpret_cast<bool*>(buf);
    *(args->bufLen) = *reinterpret_cast<uint64*>(buf + sizeof(next));
    if (*(args->bufLen) == 0) {
        *(args->buf) = nullptr;
    } else {
        char  name[MAXPATHLENGTH];
        pid_t pid = getpid();

        snprintf(name, MAXPATHLENGTH, "%s%d%d%lu", RANGEQUERYPATH, pid,
                 workerId, args->opid);
        int fd = ShmOpen(name, O_RDWR, 0777, __func__);
        *(args->buf) = static_cast<char*>(Mmap(nullptr, *(args->bufLen),
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0, __func__));
        Fclose(fd, __func__);
    }

    return next;
}

void KVWorkerClient::ClearRangeQuery(KVWorkerId workerId, RangeQueryArgs* args) {
    if (*(args->buf) && *(args->bufLen) > 0) {
        Munmap(*(args->buf), *(args->bufLen), __func__);
    }

    KVMessage sendmsg = SimpleMessage(KVOpClearRangeQuery, workerId, MyDatabaseId);
    args->opts = nullptr;
    sendmsg.ety = args;
    sendmsg.hdr.etySize = sizeof(pid_t) + sizeof(args->opid);
    sendmsg.writeFunc = WriteRangeQueryArgs;

    queue_->Send(sendmsg);
}
#endif

void KVWorkerClient::Terminate(KVWorkerId workerId) {
    queue_->Send(SimpleMessage(KVOpTerminate, workerId, MyDatabaseId));
}


/*
 * Start kv worker and begin to accept and handle requets.
 * Also it will notify kv manager and clean the resources
 * when run has finished.
 */
static void KVWorkerDo(KVWorkerId workerId, KVDatabaseId dbId) {
    KVWorker* worker = new KVWorker(workerId, dbId);
    KVManagerClient* manager = new KVManagerClient();

    worker->Start();
    /* notify ready event */
    manager->Notify(WorkerReady);

    worker->Run();

    /* notify destroyed event */
    manager->Notify(WorkerDesty);

    delete worker;
    delete manager;
}

/*
 * Entrypoint for kv worker
 */
extern "C" void KVWorkerMain(Datum arg) {
    KVDatabaseId dbId = (KVDatabaseId) DatumGetObjectId(arg);
    KVWorkerId workerId = *reinterpret_cast<KVWorkerId*>(MyBgworkerEntry->bgw_extra);

    /* Connect to our database */
    BackgroundWorkerInitializeConnectionByOid(dbId, InvalidOid, 0);

    /* Start, run and clean of kv worker */
    KVWorkerDo(workerId, dbId);
}

/*
 * Launch kv worker process
 */
void* LaunchKVWorker(KVWorkerId workerId, KVDatabaseId dbId) {
    BackgroundWorker bgw;
    memset(&bgw, 0, sizeof(bgw));
    bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
    bgw.bgw_restart_time = BGW_NEVER_RESTART;
    sprintf(bgw.bgw_library_name, "kv_fdw");
    sprintf(bgw.bgw_function_name, "KVWorkerMain");
    snprintf(bgw.bgw_name, BGW_MAXLEN, "KV Worker");
    snprintf(bgw.bgw_type, BGW_MAXLEN, "KV Worker");
    bgw.bgw_main_arg = ObjectIdGetDatum(dbId);
    memcpy(bgw.bgw_extra, &workerId, sizeof(workerId));
    /* set bgw_notify_pid so that we can use WaitForBackgroundWorkerStartup */
    bgw.bgw_notify_pid = MyProcPid;

    BackgroundWorkerHandle* handle = nullptr;
    if (!RegisterDynamicBackgroundWorker(&bgw, &handle)) {
        return nullptr;
    }

    pid_t pid;
    BgwHandleStatus status = WaitForBackgroundWorkerStartup(handle, &pid);
    if (status == BGWH_POSTMASTER_DIED) {
        ereport(WARNING, errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                errmsg("cannot start background processes without postmaster"),
                errhint("Kill all remaining database processes and restart "
                        "the database."));
    } else if (status != BGWH_STARTED) {
        ereport(WARNING, errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                errmsg("could not start background process"),
                errhint("More details may be available in the server log."));
    }

    return handle;
}
