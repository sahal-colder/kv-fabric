/*
 * 通用 KVStore 回归层，覆盖 rbtree。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "test_common.h"

int main(int argc, char *argv[]) {
	int connfd = test_connect(argc, argv);
	int failed = 0;

	if (connfd < 0) return 1;
	test_send_silent(connfd, "RDEL Teacher");

	failed += test_case(connfd, "RSET Teacher King", "OK");
	failed += test_case(connfd, "RMOD Teacher Darren", "OK");
	failed += test_case(connfd, "RGET Teacher", "Darren");
	failed += test_case(connfd, "REXIST Teacher", "EXIST");
	failed += test_case(connfd, "RDEL Teacher", "OK");
	failed += test_case(connfd, "RGET Teacher", "NO EXIST");

	return test_finish(connfd, "rbtree", failed);
}
