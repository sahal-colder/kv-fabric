/*
 * 通用 KVStore 回归层，覆盖 skiptable。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "test_common.h"

int main(int argc, char *argv[]) {
	int connfd = test_connect(argc, argv);
	int failed = 0;

	if (connfd < 0) return 1;
	test_send_silent(connfd, "SDEL Teacher");

	failed += test_case(connfd, "SSET Teacher King", "OK");
	failed += test_case(connfd, "SMOD Teacher Darren", "OK");
	failed += test_case(connfd, "SGET Teacher", "Darren");
	failed += test_case(connfd, "SEXIST Teacher", "EXIST");
	failed += test_case(connfd, "SDEL Teacher", "OK");
	failed += test_case(connfd, "SGET Teacher", "NO EXIST");

	return test_finish(connfd, "skiptable", failed);
}
