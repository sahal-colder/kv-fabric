/*
 * 通用 KVStore 回归层，覆盖 array。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "test_common.h"

#include <stdio.h>

int main(int argc, char *argv[]) {
	int connfd = test_connect(argc, argv);
	int failed = 0;

	if (connfd < 0) return 1;
	test_send_silent(connfd, "DEL Teacher");

	failed += test_case(connfd, "SET Teacher King", "OK");
	failed += test_case(connfd, "MOD Teacher Darren", "OK");
	failed += test_case(connfd, "GET Teacher", "Darren");
	failed += test_case(connfd, "EXIST Teacher", "EXIST");
	failed += test_case(connfd, "DEL Teacher", "OK");
	failed += test_case(connfd, "GET Teacher", "NO EXIST");

	return test_finish(connfd, "array", failed);
}
