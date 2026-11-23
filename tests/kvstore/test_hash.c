/*
 * 通用 KVStore 回归层，覆盖 hash。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "test_common.h"

int main(int argc, char *argv[]) {
	int connfd = test_connect(argc, argv);
	int failed = 0;

	if (connfd < 0) return 1;
	test_send_silent(connfd, "HDEL Teacher");

	failed += test_case(connfd, "HSET Teacher King", "OK");
	failed += test_case(connfd, "HMOD Teacher Darren", "OK");
	failed += test_case(connfd, "HGET Teacher", "Darren");
	failed += test_case(connfd, "HEXIST Teacher", "EXIST");
	failed += test_case(connfd, "HDEL Teacher", "OK");
	failed += test_case(connfd, "HGET Teacher", "NO EXIST");

	/* These keys share a bucket: cover middle, head, tail, and final removal. */
	failed += test_case(connfd, "HSET abc first", "OK");
	failed += test_case(connfd, "HSET acb second", "OK");
	failed += test_case(connfd, "HSET bac third", "OK");
	failed += test_case(connfd, "HSET bca fourth", "OK");
	failed += test_case(connfd, "HDEL acb", "OK");
	failed += test_case(connfd, "HGET abc", "first");
	failed += test_case(connfd, "HGET bac", "third");
	failed += test_case(connfd, "HDEL acb", "NO EXIST");
	failed += test_case(connfd, "HDEL bca", "OK");
	failed += test_case(connfd, "HDEL abc", "OK");
	failed += test_case(connfd, "HGET bac", "third");
	failed += test_case(connfd, "HDEL bac", "OK");
	failed += test_case(connfd, "HDEL bac", "NO EXIST");

	return test_finish(connfd, "hash", failed);
}
