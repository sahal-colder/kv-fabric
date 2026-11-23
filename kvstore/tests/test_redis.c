/*
 * 通用 KVStore 回归层，覆盖 redis。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "test_common.h"

int main(int argc, char *argv[]) {
	int connfd = test_connect(argc, argv);
	int failed = 0;

	if (connfd < 0) return 1;

	failed += test_case_wire(
		connfd,
		"PING\r\n",
		"+PONG\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*1\r\n$4\r\nPING\r\n",
		"+PONG\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*3\r\n$3\r\nSET\r\n$7\r\nredis-k\r\n$7\r\nredis-v\r\n",
		"+OK\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*2\r\n$3\r\nGET\r\n$7\r\nredis-k\r\n",
		"$7\r\nredis-v\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*4\r\n$3\r\nSET\r\n$7\r\nredis-k\r\n$5\r\nnewer\r\n$2\r\nXX\r\n",
		"+OK\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*3\r\n$6\r\nAPPEND\r\n$7\r\nredis-k\r\n$1\r\n!\r\n",
		":6\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*3\r\n$4\r\nMGET\r\n$7\r\nredis-k\r\n$7\r\nmissing\r\n",
		"*2\r\n$6\r\nnewer!\r\n$-1\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*3\r\n$3\r\nSET\r\n$5\r\ncount\r\n$1\r\n0\r\n",
		"+OK\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*2\r\n$4\r\nINCR\r\n$5\r\ncount\r\n",
		":1\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*2\r\n$4\r\nDECR\r\n$5\r\ncount\r\n",
		":0\r\n"
	);

	failed += test_case_wire(
		connfd,
		"*3\r\n$6\r\nINCRBY\r\n$5\r\ncount\r\n$19\r\n9223372036854775807\r\n",
		":9223372036854775807\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*2\r\n$4\r\nINCR\r\n$5\r\ncount\r\n",
		"-ERR increment or decrement would overflow\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*3\r\n$3\r\nSET\r\n$5\r\ncount\r\n$20\r\n-9223372036854775807\r\n",
		"+OK\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*2\r\n$4\r\nDECR\r\n$5\r\ncount\r\n",
		":-9223372036854775808\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*2\r\n$4\r\nDECR\r\n$5\r\ncount\r\n",
		"-ERR increment or decrement would overflow\r\n"
	);
	failed += test_case_wire(
		connfd,
		"*2\r\n$3\r\nGET\r\n$5\r\ncount\r\n",
		"$20\r\n-9223372036854775808\r\n"
	);

	return test_finish(connfd, "redis", failed);
}
