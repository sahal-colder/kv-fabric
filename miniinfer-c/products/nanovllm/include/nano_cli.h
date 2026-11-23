/*
 * 单机推理 CLI 的公开入口。
 * 只借用进程参数。CLI 调用不从持锁的存储路径进入。
 */
#ifndef NANO_CLI_H
#define NANO_CLI_H

/* 解析命令行、运行推理并输出结果。返回进程退出码。 */
int nv_cli_run(int argc, char **argv);

#endif
