#include <stdio.h>
#include <liburing.h>
int main(void){struct io_uring r;int e=io_uring_queue_init(4,&r,0);printf("io_uring_queue_init=%d\n",e);if(!e)io_uring_queue_exit(&r);return 0;}
