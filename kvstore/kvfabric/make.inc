CMAKE ?= cmake
CTEST ?= ctest
KVF_BUILD_DIR ?= $(abspath $(BUILD_DIR)/kvfabric$(if $(filter 1,$(SANITIZE)),-asan))
KVF_PREFIX ?= $(abspath $(BUILD_DIR)/kvfabric-sdk$(if $(filter 1,$(SANITIZE)),-asan))
KVF_BUILD_TYPE ?= RelWithDebInfo

.PHONY: kvfabric test_kvfabric install_kvfabric

all: kvfabric
test_unit: test_kvfabric

kvfabric:
	+$(CMAKE) -S kvfabric -B "$(KVF_BUILD_DIR)" -DCMAKE_C_COMPILER="$(CC)" \
		-DCMAKE_BUILD_TYPE="$(KVF_BUILD_TYPE)" -DKVF_BUILD_TESTS=ON \
		-DKVF_SANITIZE=$(if $(filter 1,$(SANITIZE)),ON,OFF)
	+$(CMAKE) --build "$(KVF_BUILD_DIR)"

test_kvfabric: kvfabric
	$(CTEST) --test-dir "$(KVF_BUILD_DIR)" --output-on-failure

install_kvfabric: kvfabric
	$(CMAKE) --install "$(KVF_BUILD_DIR)" --prefix "$(KVF_PREFIX)"
