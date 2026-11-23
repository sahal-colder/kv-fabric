/*
 * 通用 KVStore 回归层，覆盖 common。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#ifndef KVS_TEST_COMMON_H
#define KVS_TEST_COMMON_H

int test_connect(int argc, char *argv[]);
int test_case(int connfd, const char *input, const char *expected);
int test_case_batch(int connfd, const char *input, const char *expected);
int test_case_wire(int connfd, const char *wire_input, const char *expected);
int test_send_silent(int connfd, const char *input);
int test_finish(int connfd, const char *suite, int failed);

#endif
