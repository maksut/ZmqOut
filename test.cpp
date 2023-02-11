#include "zmq.h"
#include <stdio.h>
#include <thread>
#include <functional>
#include <iostream>
#include "test.h"
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>


#define CHECK_ZMQ_RC(OP, RET) if (rc < 0) { printf("%s failed with %d : %s\n", OP, rc, zmq_strerror(zmq_errno())); return RET; }

void workerThread(void* context, const char* workerAddress)
{
    void* workerSocket = zmq_socket(context, ZMQ_PAIR);

    int rc = zmq_connect(workerSocket, workerAddress);
    CHECK_ZMQ_RC("zmq_bind");

    while (true)
    {
        zmq_msg_t msg;
        int rc = zmq_msg_init(&msg);
        CHECK_ZMQ_RC("zmq_msg_init on worker");

        rc = zmq_msg_recv(&msg, workerSocket, 0);
        CHECK_ZMQ_RC("zmq_recv on worker");
        
        if (rc != sizeof(int8_t))
        {
            printf("Worker got an unexpected message:%.*s\n", (int)zmq_msg_size(&msg), (char*)zmq_msg_data(&msg));
            return;
        }

        printf("Worker got the topic:%d\n", *(int8_t*)zmq_msg_data(&msg));

        rc = zmq_msg_recv(&msg, workerSocket, 0);
        CHECK_ZMQ_RC("zmq_recv on worker");

        printf("Worker got the message:%.*s\n", (int)zmq_msg_size(&msg), (char*)zmq_msg_data(&msg));

        zmq_msg_close(&msg);
    }

    rc = zmq_close(workerSocket);
    CHECK_ZMQ_RC("zmq_close on worker");
    printf("worker is running\n");
}

/*
int sendMessage(void* socket, int8_t type)
{
    zmq_msg_t msg;
    
    // topic first
    int rc = zmq_send_const(socket, &PUBLISH, sizeof(PUBLISH), ZMQ_SNDMORE);
    CHECK_ZMQ_RC("zmq_msg_send in main", rc);

    // then the body
    rc = zmq_msg_init_size(&msg, 6);
    CHECK_ZMQ_RC("zmq_msg_init_size in main", rc);

    void* data = zmq_msg_data(&msg);
    memset(data, 'A', 6);
    rc = zmq_msg_send(&msg, socket, 0);
    CHECK_ZMQ_RC("zmq_msg_send in main", rc);
}
*/


int main() {
    printf("sizeof(size_t)=%d\n", sizeof(size_t));
    void* context = zmq_ctx_new();

    void* mainSocket = zmq_socket(context, ZMQ_PAIR);

    const char* address = "ipc://c:/sc/test";
    int rc = zmq_bind(mainSocket, address);
    CHECK_ZMQ_RC("zmq_bind in main", -1);

    std::thread worker = std::thread(std::bind(workerThread, context, address));

    // send a message
    //sendMessage(mainSocket, START_PUB_SOCKET);
    
    worker.join();

    rc = zmq_close(mainSocket);
    CHECK_ZMQ_RC("zmq_close on worker", -1);

    rc = zmq_ctx_destroy(context);
    CHECK_ZMQ_RC("zmq_ctx_destroy in main", -1);

    return 0;
}
