/*
 * 通用 KVStore 回归层，覆盖 common。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "test_common.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_MESSAGE_LENGTH 1024

static int send_all(int connfd, const char *message) {
	size_t length = strlen(message);
	size_t sent = 0;

	while (sent < length) {
		ssize_t result = send(connfd, message + sent, length - sent, 0);
		if (result <= 0) return -1;
		sent += (size_t)result;
	}

	return 0;
}

static int recv_response(int connfd, char *response, size_t capacity) {
	size_t used = 0;

	while (used < capacity - 1) {
		ssize_t result = recv(connfd, response + used, capacity - used - 1, 0);
		if (result <= 0) return -1;

		used += (size_t)result;
		response[used] = '\0';
		if (used >= 2 &&
			response[used - 2] == '\r' &&
			response[used - 1] == '\n') {
			return 0;
		}
	}

	return -1;
}

static int recv_expected(int connfd, char *response, size_t capacity, size_t expected_length) {
	size_t used = 0;

	if (expected_length >= capacity) return -1;
	while (used < expected_length) {
		ssize_t result = recv(connfd, response + used, expected_length - used, 0);
		if (result <= 0) return -1;
		used += (size_t)result;
	}

	response[used] = '\0';
	return 0;
}

static void trim_crlf(char *text) {
	size_t length = strlen(text);

	while (length > 0 &&
		(text[length - 1] == '\r' || text[length - 1] == '\n')) {
		text[--length] = '\0';
	}
}

static int round_trip(int connfd, const char *input, char *response) {
	char wire_message[MAX_MESSAGE_LENGTH];

	if (snprintf(
			wire_message,
			sizeof(wire_message),
			"%s\r\n",
			input
		) >= (int)sizeof(wire_message)) {
		return -1;
	}

	if (send_all(connfd, wire_message) != 0) return -1;
	return recv_response(connfd, response, MAX_MESSAGE_LENGTH);
}

int test_connect(int argc, char *argv[]) {
	int connfd;
	int port;
	struct sockaddr_in server_addr;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <ip> <port>\n", argv[0]);
		return -1;
	}

	port = atoi(argv[2]);
	if (port <= 0 || port > 65535) {
		fprintf(stderr, "invalid port: %s\n", argv[2]);
		return -1;
	}

	connfd = socket(AF_INET, SOCK_STREAM, 0);
	if (connfd < 0) {
		perror("socket");
		return -1;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons((unsigned short)port);

	if (inet_pton(AF_INET, argv[1], &server_addr.sin_addr) != 1) {
		fprintf(stderr, "invalid IPv4 address: %s\n", argv[1]);
		close(connfd);
		return -1;
	}

	if (connect(
			connfd,
			(struct sockaddr *)&server_addr,
			sizeof(server_addr)
		) != 0) {
		perror("connect");
		close(connfd);
		return -1;
	}

	return connfd;
}

int test_case(int connfd, const char *input, const char *expected) {
	char actual[MAX_MESSAGE_LENGTH] = {0};
	int result;

	if (round_trip(connfd, input, actual) != 0) {
		printf("%s ==> <transport error> = %s [FAIL]\n", input, expected);
		return 2;
	}

	trim_crlf(actual);
	result = strcmp(actual, expected) == 0;
	printf(
		"%s ==> %s = %s%s\n",
		input,
		actual,
		expected,
		result ? "" : " [FAIL]"
	);

	return result ? 0 : 1;
}

int test_case_batch(int connfd, const char *input, const char *expected) {
	char actual[MAX_MESSAGE_LENGTH] = {0};
	char wire_message[MAX_MESSAGE_LENGTH];
	size_t expected_length = strlen(expected);
	int result;

	if (expected_length >= sizeof(actual) ||
		snprintf(wire_message, sizeof(wire_message), "%s\r\n", input) >=
			(int)sizeof(wire_message) ||
		send_all(connfd, wire_message) != 0 ||
		recv_expected(connfd, actual, sizeof(actual), expected_length) != 0) {
		printf("%s ==> <transport error> = %s [FAIL]\n", input, expected);
		return 2;
	}

	result = strcmp(actual, expected) == 0;
	printf(
		"%s ==> %s = %s%s\n",
		input,
		actual,
		expected,
		result ? "" : " [FAIL]"
	);

	return result ? 0 : 1;
}

int test_case_wire(int connfd, const char *wire_input, const char *expected) {
	char actual[MAX_MESSAGE_LENGTH] = {0};
	size_t expected_length = strlen(expected);
	int result;

	if (expected_length >= sizeof(actual) ||
		send_all(connfd, wire_input) != 0 ||
		recv_expected(connfd, actual, sizeof(actual), expected_length) != 0) {
		printf("<RESP request> ==> <transport error> = %s [FAIL]\n", expected);
		return 2;
	}

	result = strcmp(actual, expected) == 0;
	printf(
		"<RESP request> ==> %s = %s%s\n",
		actual,
		expected,
		result ? "" : " [FAIL]"
	);
	return result ? 0 : 1;
}

int test_send_silent(int connfd, const char *input) {
	char response[MAX_MESSAGE_LENGTH] = {0};

	return round_trip(connfd, input, response);
}

int test_finish(int connfd, const char *suite, int failed) {
	close(connfd);
	if (failed == 0) {
		printf("%s: PASS\n", suite);
		return EXIT_SUCCESS;
	}

	printf("%s: FAIL\n", suite);
	return EXIT_FAILURE;
}
