#include "SC_PlugIn.hpp"
#include "SC_SyncCondition.h"
#include "zmq.h"
#include <iostream>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include "rw_spinlock.hpp"

// InterfaceTable contains pointers to functions in the host (server).
static InterfaceTable *ft;

#define PAYLOAD_SIZE 64
#define PUB_QUEUE_SIZE 512
#define MAX_PUB_SOCKETS 32

#define CHECK_ZMQ_RC1(OP) if (rc < 0) { Print("%s failed with %d : %s\n", OP, rc, zmq_strerror(zmq_errno())); return; }
#define CHECK_ZMQ_RC2(OP, RET) if (rc < 0) { Print("%s failed with %d : %s\n", OP, rc, zmq_strerror(zmq_errno())); return RET; }

struct Payload
{
    float data[PAYLOAD_SIZE];

    Payload()
    {
        memset(data, 0, PAYLOAD_SIZE * sizeof(float));
    }
};

class PubWorkerThread
{
    SC_SyncCondition mHasData;

#ifdef SUPERNOVA
    boost::lockfree::queue<Payload, boost::lockfree::capacity<PUB_QUEUE_SIZE>> mPubQueue;
#else
    boost::lockfree::spsc_queue<Payload, boost::lockfree::capacity<PUB_QUEUE_SIZE>> mPubQueue;
#endif

    std::atomic<bool> mRunning;
    SC_Thread mThread;

public:
    PubWorkerThread() :mRunning(false) {}

    ~PubWorkerThread()
    {
        if (mRunning)
        {
            Print("Thread is stopping\n");
            mRunning.store(false);
            mHasData.Signal();
            mThread.join();
            std::cout << "Thread is stopped\n";
        }
    }

    void launchThread(void* zmqContext, const char* address)
    {
        mThread = std::thread(std::bind(&PubWorkerThread::ioThreadFunc, this, zmqContext, address));
    }

    bool send(Payload& payload) 
    {
        bool pushSucceeded = mPubQueue.push(payload);

        if (pushSucceeded)
            mHasData.Signal();

        return pushSucceeded;
    }

private:
    void ioThreadFunc(void* zmqContext, const char* address) 
    {
        Print("Running the thread\n");
        mRunning.store(true);

        void* pubSocket = zmq_socket(zmqContext, ZMQ_PUB);

        int rc = zmq_bind(pubSocket, address);
        CHECK_ZMQ_RC1("zmq_bind in ioThreadFunc");

        while (mRunning.load()) 
        {
            Print("Waiting for new data\n");
            mHasData.WaitEach();
            Print("Got a signal\n");

            // Consumes all avaiable tasks from the queue, one at a time
            Payload payload;
            while (mPubQueue.pop(&payload, 1))
            {
                Print("publishing payload\n");

                size_t msgSize = sizeof(payload.data);
                void* msg = malloc(msgSize);
                
                if (!msg)
                    continue;

                memcpy(msg, payload.data, msgSize);

                int rc = zmq_send(pubSocket, msg, msgSize, ZMQ_NOBLOCK);
                CHECK_ZMQ_RC1("zmq_send on payload send");

                Print("publishing payload end\n");
            }
        }

        rc = zmq_close(pubSocket);
        CHECK_ZMQ_RC1("zmq_close in ioThreadFunc");

        Print("Stopping the thread\n");
    }
};

class ThreadContainer
{
    PubWorkerThread* threads[MAX_PUB_SOCKETS];
    void* zmqContext = nullptr;
    rw_spinlock lock[MAX_PUB_SOCKETS];

public:
    void start(int index, const char* address)
    {
        rw_spinlock::scoped_lock _(lock[index]);

        if (threads[index])
            delete threads[index];

        threads[index] = new PubWorkerThread();

        if (!zmqContext)
            zmqContext = zmq_ctx_new();

        threads[index]->launchThread(zmqContext, address);
    }

    void stop(int index)
    {
        rw_spinlock::scoped_lock _(lock[index]);

        if (threads[index])
            delete threads[index];

        threads[index] = nullptr;
    }

    void send(int index, Payload& payload)
    {
        if (!lock[index].try_lock_shared())
            return;

        if (threads[index])
            threads[index]->send(payload);

        lock[index].unlock_shared();
    }

    ~ThreadContainer()
    {
        Print("~ThreadContainer start\n");

        for (int i = 0; i < MAX_PUB_SOCKETS; ++i)
        {
            rw_spinlock::scoped_lock _(lock[i]);

            if (threads[i])
                delete threads[i];
        }

        Print("Destroying zmq context\n");
        zmq_ctx_destroy(zmqContext);
        Print("~ThreadContainer end\n");
    }
};

struct PluginData // data for the global instance of the plugin
{
    ThreadContainer* threads = nullptr;
};

PluginData gPlugin; // global

// declare struct to hold unit generator state
struct ZmqOut : public SCUnit {
// Constructor usually does 3 things.
// 1. set the calculation function.
// 2. initialize the unit generator state variables.
// 3. calculate one sample of output.
public:
    ZmqOut() {
        Print("ZmqPub constructor\n");

        // Initialize the unit generator state variables.
        index = 0; // TODO: fetch from args
        payloadIndex = 0;

        // Set the calculation function. set_calc_function also computes the initial sample
        if (isAudioRateIn(0))
            // Audio rate
            set_calc_function<ZmqOut,&ZmqOut::next_a>();
        else // Control rate (or a scalar)
            set_calc_function<ZmqOut,&ZmqOut::next_k>();
    }

private:
    Payload payload;
    int payloadIndex;
    int index;

    // The calculation function executes once per control period
    // which is typically 64 samples.

    // calculation function for an audio rate frequency argument
    void next_a(int inNumSamples)
    {
        // get the pointer to the output buffer
        float *outBuf = out(0);

        // get the pointer to the input buffer
        const float *inBuf = in(0);

        // perform a loop for the number of samples in the control period.
        // If this unit is audio rate then inNumSamples will be 64 or whatever
        // the block size is. If this unit is control rate then inNumSamples will
        // be 1.
        for (int i = 0; i < inNumSamples; ++i)
        {
            float in = inBuf[i];

            // write the buffer
            stream(in);

            // write the output
            outBuf[i] = in;
        }
    }

    // calculation function for a control rate frequency argument
    void next_k(int inNumSamples)
    {
        // get the pointer to the output buffer
        float *outBuf = out(0);

        // in is control rate, so calculate it once.
        float in = in0(0);

        // sending one sample per rate
        stream(in);

        for (int i=0; i < inNumSamples; ++i)
            outBuf[i] = in;
    }

    inline void stream(float sample)
    {
        if (!gPlugin.threads)
            return;

        payload.data[payloadIndex++] = sample;

        if (payloadIndex >= PAYLOAD_SIZE)
        {
            gPlugin.threads->send(index, payload);
            payloadIndex = 0;
        }
    }
};

struct CmdData // data for each command
{
    PluginData* myPlugin;
    int index;
    char* address;
};

bool cmdStage2(World* world, void* inUserData) 
{
    // user data is the command.
    CmdData* cmdData = (CmdData*)inUserData;

    // just print out the values
    Print("cmdStage2 index %d addres %s\n", cmdData->index, cmdData->address);

    // ZMQ TEST
    Print("ZmqPub plugin load\n");

    if (!gPlugin.threads)
        gPlugin.threads = new ThreadContainer();

    gPlugin.threads->start(cmdData->index, cmdData->address);

    // TODO: wait for a reply from START_PUB_SOCKET

    return true;
}

bool cmdStage3(World* world, void* inUserData) 
{
    // user data is the command.
    CmdData* cmdData = (CmdData*)inUserData;

    // just print out the values
    Print("cmdStage3 index %d addres %s\n", cmdData->index, cmdData->address);

    // scsynth will perform completion message after this returns
    return true;
}

bool cmdStage4(World* world, void* inUserData) {
    // user data is the command.
    CmdData* cmdData = (CmdData*)inUserData;

    // just print out the values
    Print("cmdStage4 index %d addres %s\n", cmdData->index, cmdData->address);

    // scsynth will send /done after this returns
    return true;
}

void cmdCleanup(World* world, void* inUserData) 
{
    // user data is the command.
    CmdData* cmdData = (CmdData*)inUserData;

    Print("cmdCleanup index %d addres %s\n", cmdData->index, cmdData->address);

    RTFree(world, cmdData->address); // free the string
    RTFree(world, cmdData); // free command data
    // scsynth will delete the completion message for you.
}

void cmdZmqOut(World* inWorld, void* inUserData, struct sc_msg_iter* args, void* replyAddr) 
{
    Print("->cmdZmqOut %p\n", inUserData);

    // user data is the plug-in's user data.
    PluginData* plugInData = (PluginData*)inUserData;

    // allocate command data, free it in cmdCleanup.
    CmdData* cmdData = (CmdData*)RTAlloc(inWorld, sizeof(CmdData));
    if (!cmdData) 
    {
        Print("cmdZmqOut: memory allocation failed!\n");
        return;
    }

    cmdData->myPlugin = plugInData;
    cmdData->address = "tcp://*:5555";

    // ..get data from args..
    cmdData->index = args->geti(0); // default index is 0

    const char* address = args->gets(); // get the string argument
    if (address) 
    {
        cmdData->address = (char*)RTAlloc(inWorld, strlen(address) + 1); // allocate space, free it in cmdCleanup.
        if (!cmdData->address) 
        {
            Print("cmdZmqOut: memory allocation failed!\n");
            return;
        }

        #pragma warning(suppress: 4996)
        strcpy(cmdData->address, address); // copy the string
    }

    DoAsynchronousCommand(inWorld, replyAddr, "cmdDemoFunc", cmdData, cmdStage2, cmdStage3, cmdStage4, cmdCleanup, 0, nullptr);

    Print("<-cmdDemoFunc\n");
}

/*
 * to test the above, send the server these commands:
 *
 * s.sendMsg(\cmd, \zmqOut, 0, "tcp://*:5555");
 * s.sendMsg(\cmd, \zmqOut, 0);
 * s.sendMsg(\cmd, \zmqOut);
 *
 */

C_LINKAGE SC_API_EXPORT void unload(InterfaceTable* inTable)
{
    delete gPlugin.threads;
}

// the entry point is called by the host when the plug-in is loaded
PluginLoad(ZmqPubUGens) 
{
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable

    // registerUnit takes the place of the Define*Unit functions. It automatically checks for the presence of a
    // destructor function.
    // However, it does not seem to be possible to disable buffer aliasing with the C++ header.
    registerUnit<ZmqOut>(ft, "ZmqOut");

    // define a plugin command
    DefinePlugInCmd("zmqOut", cmdZmqOut, (void*)&gPlugin);
}