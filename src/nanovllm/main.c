/*
 * nano-vLLM 的启动入口，将参数交给推理 CLI。
 * 资源由 CLI 管理。入口不持有锁。
 */
#include "nano_cli.h"

int main(int argc, char **argv) {
    return nv_cli_run(argc, argv);
}
