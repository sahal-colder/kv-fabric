/*
 * 通用 KVStore 回归层，覆盖 protocol。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "test_common.h"

int main(int argc, char *argv[]) {
	int connfd = test_connect(argc, argv);
	int failed = 0;

	if (connfd < 0) return 1;

	failed += test_case(connfd, "UNKNOWN Teacher", "ERROR");
	failed += test_case(connfd, "SET Teacher", "ERROR");
	failed += test_case(connfd, "GET Teacher extra", "ERROR");
	failed += test_case_batch(
		connfd,
		"SET BatchTeacher King\r\nGET BatchTeacher\r\nDEL BatchTeacher",
		"OK\r\nKing\r\nOK\r\n"
	);

	return test_finish(connfd, "protocol", failed);
}
