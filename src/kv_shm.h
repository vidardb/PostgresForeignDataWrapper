
#ifndef SRC_KV_SHM_H_
#define SRC_KV_SHM_H_

#include "kv_fdw.h"


ManagerSharedMem *InitManagerSharedMem(void);

void CloseManagerSharedMem(ManagerSharedMem *manager);

void KVWorkerMain(WorkerProcKey *workerKey);

void TerminateWorker(WorkerProcKey *workerKey);


#endif /* SRC_KV_SHM_H_ */
