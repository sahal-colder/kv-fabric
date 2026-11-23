CC := gcc

ALLOCATOR ?= libc
RDMA ?= 0
BPF ?= 0
MYSQL ?= 0
NTYCO ?= 0
PROACTOR ?= 1
SANITIZE ?= 0
KVFABRIC ?= 0

ifeq ($(ALLOCATOR),jemalloc)
ALLOCATOR_CFLAGS := -DKVS_ALLOCATOR_BACKEND=1
ALLOCATOR_LDLIBS := -ljemalloc
else ifeq ($(ALLOCATOR),libc)
ALLOCATOR_CFLAGS := -DKVS_ALLOCATOR_BACKEND=0
ALLOCATOR_LDLIBS :=
else ifeq ($(ALLOCATOR),mmpool)
ALLOCATOR_CFLAGS := -DKVS_ALLOCATOR_BACKEND=2
ALLOCATOR_LDLIBS :=
else
$(error ALLOCATOR must be jemalloc, libc, or mmpool)
endif

TARGET_NAME := kvstore
TEST_NAMES := test_array test_rbtree test_hash test_skiptable test_protocol test_redis test_replication test_snapshot_resp
UNIT_NAMES := test_config test_sync_wire
THIRD_PARTY_DIR := third_party/NtyCo
INCLUDE_DIR := include
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj/$(ALLOCATOR)-rdma$(RDMA)-bpf$(BPF)-mysql$(MYSQL)-ntyco$(NTYCO)-proactor$(PROACTOR)-san$(SANITIZE)

TARGET := $(BIN_DIR)/$(TARGET_NAME)
TEST_TARGETS := $(addprefix $(BIN_DIR)/,$(TEST_NAMES))
ALLOCATOR_TEST_TARGET := $(BIN_DIR)/test_allocator

SRCS := src/main.c src/config/kvs_config.c \
        src/network/kvs_connection.c \
        src/engine/kvs_engine.c \
        src/protocol/kvs_protocol.c src/protocol/kvs_resp.c src/protocol/kvs_text.c \
		src/protocol/kvs_format.c \
        src/redis/kvs_redis.c \
        src/memory/kvs_allocator.c src/memory/kvs_mmpool.c \
        src/store/kvs_array.c src/store/kvs_rbtree.c src/store/kvs_hash.c \
        src/store/kvs_skiptable.c src/store/kvs_persistence.c \
		src/store/kvs_incremental.c src/store/kvs_mysql.c \
		src/store/kvs_backend.c src/store/kvs_lock.c src/store/kvs_data.c src/store/kvs_file.c \
		src/replication/kvs_replication.c src/replication/kvs_master.c \
		src/replication/kvs_slave.c src/replication/kvs_transport.c \
		src/replication/kvs_sync_wire.c src/replication/kvs_bpf.c \
		src/network/reactor.c

ifeq ($(PROACTOR),1)
SRCS += src/network/proactor.c
endif
ifeq ($(NTYCO),1)
SRCS += src/network/ntyco.c
endif

OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))

CFLAGS := -std=c11 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -MMD -MP \
          -I $(INCLUDE_DIR) -I $(THIRD_PARTY_DIR)/core $(ALLOCATOR_CFLAGS) \
          -DKVS_ENABLE_RDMA=$(RDMA) -DKVS_ENABLE_BPF=$(BPF) -DENABLE_MYSQL=$(MYSQL) \
          -DKVS_ENABLE_NTYCO=$(NTYCO) -DKVS_ENABLE_PROACTOR=$(PROACTOR)
LDFLAGS := -L $(THIRD_PARTY_DIR)
ifeq ($(SANITIZE),1)
ifneq ($(ALLOCATOR),libc)
$(error SANITIZE=1 requires ALLOCATOR=libc)
endif
CFLAGS += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie
LDFLAGS += -fsanitize=address,undefined -no-pie
endif
LDLIBS := -lpthread -ldl $(ALLOCATOR_LDLIBS)
ifeq ($(PROACTOR),1)
LDLIBS += -luring
endif
ifeq ($(MYSQL),1)
LDLIBS += -lmysqlclient
endif
ifeq ($(RDMA),1)
LDLIBS += -lrdmacm -libverbs
endif
ifeq ($(BPF),1)
LDLIBS += -lbpf -lelf -lz
endif
ifeq ($(NTYCO),1)
LDLIBS += -lntyco
TARGET_DEPS := ntyco
endif

.PHONY: all ntyco clean test_allocator test_unit bpf FORCE $(TARGET_NAME) $(TEST_NAMES) $(UNIT_NAMES)

all: $(TARGET_NAME) $(TEST_NAMES) $(ALLOCATOR_TEST_TARGET) $(UNIT_NAMES)

ifeq ($(KVFABRIC),1)
include cmake/kvfabric.mk
endif

ntyco:
	$(MAKE) -C $(THIRD_PARTY_DIR)

$(TARGET_NAME): $(TARGET)

$(TEST_NAMES): %: $(BIN_DIR)/%

test_allocator: $(ALLOCATOR_TEST_TARGET)
	$(ALLOCATOR_TEST_TARGET)

$(TARGET): $(OBJS) | $(TARGET_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

$(filter-out $(BIN_DIR)/test_snapshot_resp,$(TEST_TARGETS)): $(BIN_DIR)/%: $(OBJ_DIR)/tests/%.o $(OBJ_DIR)/tests/test_common.o
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(filter-out FORCE,$^) $(LDFLAGS)

$(BIN_DIR)/test_replication: $(OBJ_DIR)/replication/kvs_replication.o

$(BIN_DIR)/test_snapshot_resp: $(OBJ_DIR)/protocol/kvs_format.o \
		$(OBJ_DIR)/protocol/kvs_resp.o $(OBJ_DIR)/memory/kvs_allocator.o \
		$(OBJ_DIR)/memory/kvs_mmpool.o $(OBJ_DIR)/store/kvs_array.o \
		$(OBJ_DIR)/config/kvs_config.o $(OBJ_DIR)/replication/kvs_replication.o \
		$(OBJ_DIR)/store/kvs_rbtree.o $(OBJ_DIR)/store/kvs_hash.o \
		$(OBJ_DIR)/store/kvs_skiptable.o $(OBJ_DIR)/store/kvs_data.o \
		$(OBJ_DIR)/store/kvs_lock.o $(OBJ_DIR)/store/kvs_incremental.o \
		$(OBJ_DIR)/store/kvs_file.o $(OBJ_DIR)/store/kvs_persistence.o \
		$(OBJ_DIR)/store/kvs_backend.o $(OBJ_DIR)/store/kvs_mysql.o $(OBJ_DIR)/tests/test_snapshot_resp.o
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(filter-out FORCE,$^) $(LDFLAGS) $(filter-out -lntyco,$(LDLIBS))

$(UNIT_NAMES): %: $(BIN_DIR)/%

$(BIN_DIR)/test_config: $(OBJ_DIR)/tests/test_config.o $(OBJ_DIR)/config/kvs_config.o $(OBJ_DIR)/replication/kvs_replication.o
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(filter-out FORCE,$^) $(LDFLAGS)

$(BIN_DIR)/test_sync_wire: $(OBJ_DIR)/tests/test_sync_wire.o \
		$(OBJ_DIR)/replication/kvs_transport.o $(OBJ_DIR)/replication/kvs_sync_wire.o \
		$(OBJ_DIR)/replication/kvs_replication.o $(OBJ_DIR)/protocol/kvs_format.o \
		$(OBJ_DIR)/protocol/kvs_resp.o $(OBJ_DIR)/memory/kvs_allocator.o $(OBJ_DIR)/memory/kvs_mmpool.o
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(filter-out FORCE,$^) $(LDFLAGS) $(filter-out -lntyco -lmysqlclient,$(LDLIBS))

test_unit: $(UNIT_NAMES) test_replication test_snapshot_resp
	$(BIN_DIR)/test_config
	$(BIN_DIR)/test_sync_wire
	$(BIN_DIR)/test_replication
	$(BIN_DIR)/test_snapshot_resp

CLANG ?= clang
BPF_OBJ := $(BUILD_DIR)/bpf/kvs_sockhash.bpf.o
bpf: $(BPF_OBJ)

$(BPF_OBJ): src/bpf/kvs_sockhash.bpf.c include/kvs_bpf_shared.h
	@mkdir -p $(dir $@)
	$(CLANG) -target bpf -std=c11 -Dasm=__asm__ -Dtypeof=__typeof__ -O2 -g -Wall -Werror \
		-I include -I /usr/include/$(shell $(CC) -dumpmachine) -c $< -o $@

-include $(OBJS:.o=.d)
-include $(wildcard $(OBJ_DIR)/tests/*.d)

# Shared output names always link the selected feature/allocator objects.
FORCE:
$(TARGET) $(TEST_TARGETS) $(addprefix $(BIN_DIR)/,$(UNIT_NAMES)) $(ALLOCATOR_TEST_TARGET): FORCE

$(ALLOCATOR_TEST_TARGET): $(OBJ_DIR)/tests/test_allocator.o \
		$(OBJ_DIR)/memory/kvs_allocator.o $(OBJ_DIR)/memory/kvs_mmpool.o
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(filter-out FORCE,$^) $(LDFLAGS) -lpthread $(ALLOCATOR_LDLIBS)

$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/tests/%.o: tests/kvstore/%.c tests/kvstore/test_common.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I tests/kvstore -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET_NAME) $(TEST_NAMES)
	@if [ -f "$(THIRD_PARTY_DIR)/Makefile" ]; then $(MAKE) -C $(THIRD_PARTY_DIR) clean; fi
