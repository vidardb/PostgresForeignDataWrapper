
MODULE_big   = kv_fdw

ifdef VIDARDB
PG_CPPFLAGS += -Wno-declaration-after-statement -DVIDARDB
SHLIB_LINK   = -lvidardb

COMPILE.cxx.bc = $(CLANG) -xc++ -Wno-ignored-attributes $(BITCODE_CXXFLAGS) $(CPPFLAGS) -emit-llvm -c

%.bc : %.cpp
	$(COMPILE.cxx.bc) -o $@ $<
	$(LLVM_BINPATH)/opt -module-summary -f $@ -o $@

else
PG_CPPFLAGS += -Wno-declaration-after-statement
SHLIB_LINK   = -lrocksdb
endif

OBJS         = src/kv_fdw.o src/kv_utility.o src/kv_shm.o src/kv_storage.o src/kv_posix.o

EXTENSION    = kv_fdw
DATA         = sql/kv_fdw--0.0.1.sql


# Users need to specify their own path
PG_CONFIG    = /usr/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Users can specify their own configuration
REGISTRY ?= vidardb
TAG ?= rocksdb-6.2.4
IMAGE ?= postgresql
DOCKER ?= docker

src/kv_storage.bc:
	$(COMPILE.cxx.bc) $(CCFLAGS) $(CPPFLAGS) -fPIC -c -o $@ src/kv_storage.cc


.PHONY: docker-image
docker-image:
	@echo "Building docker image..."
	$(DOCKER) build --no-cache --pull -t $(REGISTRY)/$(IMAGE):$(TAG) docker_image

indent:
	@echo "Runing pgindent for format code..."
	./src/tools/pgindent/check-indent.sh

