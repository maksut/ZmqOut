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

#ifdef NDEBUG
#define Print(...)
#endif

struct Payload
{
    float data[PAYLOAD_SIZE];
};

struct VPayload
{
    void* data;
    size_t dataSize;
    zmq_free_fn* freeFn;

    VPayload(void* data, size_t dataSize, zmq_free_fn* freeFn):data(data), dataSize(dataSize), freeFn(freeFn){}
};

class PubWorkerThread
{
    SC_SyncCondition mHasData;

#ifdef SUPERNOVA
    boost::lockfree::queue<Payload, boost::lockfree::capacity<PUB_QUEUE_SIZE>> mPubQueue;
    boost::lockfree::queue<VPayload, boost::lockfree::capacity<PUB_QUEUE_SIZE>> mVPubQueue;
#else
    boost::lockfree::spsc_queue<Payload, boost::lockfree::capacity<PUB_QUEUE_SIZE>> mPubQueue;
    boost::lockfree::spsc_queue<VPayload, boost::lockfree::capacity<PUB_QUEUE_SIZE>> mVPubQueue;
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

    bool vSend(VPayload& payload)
    {
        bool pushSucceeded = mVPubQueue.push(payload);

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

        Payload payload;
        VPayload vPayload(nullptr, 0, nullptr);

        while (mRunning.load()) 
        {
            Print("Waiting for new data\n");
            mHasData.WaitEach();
            Print("Got a signal\n");

            if (mPubQueue.pop(&payload, 1))
            {
                Print("publishing payload\n");

                int rc = zmq_send(pubSocket, payload.data, sizeof(payload.data), ZMQ_NOBLOCK);
                CHECK_ZMQ_RC1("zmq_send on payload send");

                Print("publishing payload end\n");
            }
            else if (mVPubQueue.pop(&vPayload, 1))
            {
                Print("publishing vpayload\n");

                zmq_msg_t msg;
                int rc = zmq_msg_init_data(&msg, vPayload.data, vPayload.dataSize, vPayload.freeFn, nullptr);
                CHECK_ZMQ_RC1("zmq_send on zmq_msg_init_data");

                rc = zmq_msg_send(&msg, pubSocket, ZMQ_NOBLOCK);
                CHECK_ZMQ_RC1("zmq_send on vpayload send");

                Print("publishing vpayload end\n");
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

    void vSend(int index, VPayload& vPayload)
    {
        if (!lock[index].try_lock_shared())
            return;

        if (threads[index])
            threads[index]->vSend(vPayload);

        lock[index].unlock_shared();
    }

    ~ThreadContainer()
    {
        Print("~ThreadContainer start\n");

        for (int i = 0; i < MAX_PUB_SOCKETS; ++i)
        {
            rw_spinlock::scoped_lock _(lock[i]);
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

    // The calculation function executes once per control period
    // which is typically 64 samples.

    // calculation function for an audio rate frequency argument
    void next_a(int inNumSamples)
    {
        // get the pointer to the output buffer
        float *outBuf = out(0);

        // get the pointer to the input buffer
        const float *inBuf = in(0);

        // socket index
        const int socketIndex = (int)in0(1);

        // perform a loop for the number of samples in the control period.
        // If this unit is audio rate then inNumSamples will be 64 or whatever
        // the block size is. If this unit is control rate then inNumSamples will
        // be 1.
        for (int i = 0; i < inNumSamples; ++i)
        {
            float in = inBuf[i];

            // write the buffer
            stream(in, socketIndex);

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
        int socketIndex = (int)in0(1);

        // sending one sample per rate
        stream(in, socketIndex);

        for (int i=0; i < inNumSamples; ++i)
            outBuf[i] = in;
    }

    inline void stream(float sample, int socketIndex)
    {
        if (!gPlugin.threads)
            return;

        payload.data[payloadIndex++] = sample;

        if (payloadIndex >= PAYLOAD_SIZE)
        {
            gPlugin.threads->send(socketIndex, payload);
            payloadIndex = 0;
        }
    }
};

struct CmdData // data for each command
{
    PluginData* myPlugin;
    char* cmd;
    int index;
    char* address;
    int bufferNum;
};

void freeFn(void* data, void* hint)
{
    free(data);
}

void bufferStream(World* world, int socketIndex, int bufferNum)
{
    void* data = nullptr;
    size_t dataSize = 0;

    SndBuf* buf = World_GetNRTBuf(world, bufferNum);

    // only single channel buffers for now
    // and with a single big chunk
    if (buf && buf->data && buf->channels == 1) 
    {
        Print("Copying buffer data\n");
        dataSize = (buf->frames) * sizeof(float);
        data = malloc(dataSize);

        if (data)
            memcpy(data, buf->data, dataSize);
    }
    else
        Print("Couldn't get buffer %d\n", bufferNum);

    if (data)
    {
        Print("Sending buffer data\n");
        VPayload vPayload(data, dataSize, freeFn);
        gPlugin.threads->vSend(socketIndex, vPayload);
    }
    else
        Print("No buffer data to send\n");
}

bool cmdStage2(World* world, void* inUserData) 
{
    // user data is the command.
    CmdData* cmdData = (CmdData*)inUserData;

    // just print out the values
    Print("cmdStage2 cmd %s index %d addres %s bufferNum %d\n", cmdData->cmd, cmdData->index, cmdData->address, cmdData->bufferNum);

    // create a thread container if there's none
    if (!gPlugin.threads)
        gPlugin.threads = new ThreadContainer();

    if (strcmp(cmdData->cmd, "start") == 0)
        gPlugin.threads->start(cmdData->index, cmdData->address);
    else if (strcmp(cmdData->cmd, "stop") == 0)
        gPlugin.threads->stop(cmdData->index);
    else if (strcmp(cmdData->cmd, "streamBuffer") == 0)
        bufferStream(world, cmdData->index, cmdData->bufferNum);

    return true;
}

bool cmdStage3(World* world, void* inUserData) 
{
    // user data is the command.
    CmdData* cmdData = (CmdData*)inUserData;

    // just print out the values
    Print("cmdStage3 cmd %s index %d addres %s bufferNum %d\n", cmdData->cmd, cmdData->index, cmdData->address, cmdData->bufferNum);

    // scsynth will perform completion message after this returns
    return true;
}

bool cmdStage4(World* world, void* inUserData) {
    // user data is the command.
    CmdData* cmdData = (CmdData*)inUserData;

    // just print out the values
    Print("cmdStage4 cmd %s index %d addres %s bufferNum %d\n", cmdData->cmd, cmdData->index, cmdData->address, cmdData->bufferNum);

    // scsynth will send /done after this returns
    return true;
}

void cmdCleanup(World* world, void* inUserData) 
{
    // user data is the command.
    CmdData* cmdData = (CmdData*)inUserData;

    Print("cmdCleanup cmd %s index %d addres %s bufferNum %d\n", cmdData->cmd, cmdData->index, cmdData->address, cmdData->bufferNum);

    RTFree(world, cmdData->cmd);
    RTFree(world, cmdData->address);
    RTFree(world, cmdData);
}

char* getStringArg(World* world, struct sc_msg_iter* args, const char* default)
{
    const char* source = args->gets(default);
    char* dest = (char*)RTAlloc(world, strlen(source) + 1); // allocate space, free it in cmdCleanup.

    if (!dest)
    {
        Print("cmdZmqOut: memory allocation failed!\n");
        return nullptr;
    }

    #pragma warning(suppress: 4996)
    strcpy(dest, source); // copy the string

    return dest;
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

    // ..get data from args..
    cmdData->cmd = getStringArg(inWorld, args, "start");
    cmdData->index = args->geti(0); // default index is 0

    if (strcmp(cmdData->cmd, "streamBuffer") == 0)
    {
        cmdData->address = nullptr;
        cmdData->bufferNum = args->geti(0); // default bufferNum is 0
    }
    else
    {
        cmdData->address = getStringArg(inWorld, args, "tcp://*:5555");
        cmdData->bufferNum = 0;
    }

    DoAsynchronousCommand(inWorld, replyAddr, "zmqOut", cmdData, cmdStage2, cmdStage3, cmdStage4, cmdCleanup, 0, nullptr);

    Print("<-cmdDemoFunc\n");
}

/*
 * to test the above, send the server these commands:
 *
 * s.sendMsg(\cmd, \zmqOut, \start, 0, "tcp://*:5555");
 * s.sendMsg(\cmd, \zmqOut, \start, 0);
 * s.sendMsg(\cmd, \zmqOut, \start);
 * s.sendMsg(\cmd, \zmqOut);
 *
 * s.sendMsg(\cmd, \zmqOut, \stop, 0);
 * 
 * s.sendMsg(\cmd, \zmqOut, \streamBuffer, 0, 0);
 * s.sendMsg(\cmd, \zmqOut, \streamBuffer);
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
