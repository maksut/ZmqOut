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
        CHECK_ZMQ_RC1("zmq_bind in startPubSocket");

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

                int rc = zmq_send(pubSocket, payload.data, sizeof(payload.data), ZMQ_NOBLOCK);
                CHECK_ZMQ_RC1("zmq_send on payload send");

                Print("publishing payload end\n");
            }
        }

        zmq_close(pubSocket);
        Print("Stopping the thread\n");
    }
};

class ThreadContainer
{
    PubWorkerThread* threads[MAX_PUB_SOCKETS];
    void* zmqContext = nullptr;
    rw_spinlock lock;

public:
    void start(int index, const char* address)
    {
        rw_spinlock::scoped_lock _(lock);

        if (threads[index])
            delete threads[index];

        threads[index] = new PubWorkerThread();

        if (!zmqContext)
            zmqContext = zmq_ctx_new();

        threads[index]->launchThread(zmqContext, address);
    }

    void stop(int index)
    {
        rw_spinlock::scoped_lock _(lock);

        if (threads[index])
            delete threads[index];

        threads[index] = nullptr;
    }

    void send(int index, Payload& payload)
    {
        if (!lock.try_lock_shared())
            return;

        if (threads[index])
            threads[index]->send(payload);

        lock.unlock_shared();
    }

    ~ThreadContainer()
    {
        rw_spinlock::scoped_lock _(lock);

        for (int i = 0; i < MAX_PUB_SOCKETS; ++i)
        {
            if (threads[i])
                delete threads[i];
        }

        zmq_ctx_destroy(zmqContext);
    }
};

ThreadContainer* gThreadContainer = nullptr;

// declare struct to hold unit generator state
struct ZmqPub : public SCUnit {
// Constructor usually does 3 things.
// 1. set the calculation function.
// 2. initialize the unit generator state variables.
// 3. calculate one sample of output.
public:
    ZmqPub() {
        Print("ZmqPub constructor\n");

        // Initialize the unit generator state variables.
        index = 0; // TODO: fetch from args
        payloadIndex = 0;

        // Set the calculation function. set_calc_function also computes the initial sample
        if (isAudioRateIn(0))
            // Audio rate
            set_calc_function<ZmqPub,&ZmqPub::next_a>();
        else // Control rate (or a scalar)
            set_calc_function<ZmqPub,&ZmqPub::next_k>();
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
        if (!gThreadContainer)
            return;

        payload.data[payloadIndex++] = sample;

        if (payloadIndex > PAYLOAD_SIZE)
        {
            gThreadContainer->send(index, payload);
            payloadIndex = 0;
        }
    }
};


// example of implementing a plug in command with async execution.

struct MyPluginData // data for the global instance of the plugin
{
    float a, b;
};

struct MyCmdData // data for each command
{
    MyPluginData* myPlugin;
    float x, y;
    char* name;
};

MyPluginData gMyPlugin; // global

bool cmdStage2(World* world, void* inUserData) {
    // user data is the command.
    MyCmdData* myCmdData = (MyCmdData*)inUserData;

    // just print out the values
    Print("cmdStage2 a %g  b %g  x %g  y %g  name %s\n", myCmdData->myPlugin->a, myCmdData->myPlugin->b, myCmdData->x,
        myCmdData->y, myCmdData->name);

    // ZMQ TEST
    Print("ZmqPub plugin load\n");

    // DiskIO_UGens does a similar allocation on heap during plugin load. So it must be OK.
    char* address = myCmdData->name; // TODO: rename name
    int index = (int)myCmdData->x; // TODO: rename x

    if (!gThreadContainer)
        gThreadContainer = new ThreadContainer();

    gThreadContainer->start(index, address);

    // Send a ConnectTask so our thread binds a zmq port.
    // But wait for it to complete so we can capture the context and publisher socket.

    /*
    Task task(START_PUB_SOCKET, 0, address, strlen(address)+1);
    gWorkerThread->send(task);

    // now the worker thread owns the address
    myCmdData->name = 0;
    */

    // TODO: wait for a reply from START_PUB_SOCKET

    return true;
}

bool cmdStage3(World* world, void* inUserData) {
    // user data is the command.
    MyCmdData* myCmdData = (MyCmdData*)inUserData;

    // just print out the values
    Print("cmdStage3 a %g  b %g  x %g  y %g  name %s\n", myCmdData->myPlugin->a, myCmdData->myPlugin->b, myCmdData->x,
        myCmdData->y, myCmdData->name);

    // scsynth will perform completion message after this returns
    return true;
}

bool cmdStage4(World* world, void* inUserData) {
    // user data is the command.
    MyCmdData* myCmdData = (MyCmdData*)inUserData;

    // just print out the values
    Print("cmdStage4 a %g  b %g  x %g  y %g  name %s\n", myCmdData->myPlugin->a, myCmdData->myPlugin->b, myCmdData->x,
        myCmdData->y, myCmdData->name);

    // scsynth will send /done after this returns
    return true;
}

void cmdCleanup(World* world, void* inUserData) {
    // user data is the command.
    MyCmdData* myCmdData = (MyCmdData*)inUserData;

    Print("cmdCleanup a %g  b %g  x %g  y %g  name %s\n", myCmdData->myPlugin->a, myCmdData->myPlugin->b, myCmdData->x,
        myCmdData->y, myCmdData->name);

    RTFree(world, myCmdData->name); // free the string
    RTFree(world, myCmdData); // free command data
    // scsynth will delete the completion message for you.
}

void cmdZmqPub(World* inWorld, void* inUserData, struct sc_msg_iter* args, void* replyAddr) {
    Print("->cmdZmqPub %p\n", inUserData);

    // user data is the plug-in's user data.
    MyPluginData* thePlugInData = (MyPluginData*)inUserData;

    // allocate command data, free it in cmdCleanup.
    MyCmdData* myCmdData = (MyCmdData*)RTAlloc(inWorld, sizeof(MyCmdData));
    if (!myCmdData) {
        Print("cmdZmqPub: memory allocation failed!\n");
        return;
    }
    myCmdData->myPlugin = thePlugInData;

    // ..get data from args..
    myCmdData->x = 0.;
    myCmdData->y = 0.;
    myCmdData->name = 0;

    // float arguments
    myCmdData->x = args->getf();
    myCmdData->y = args->getf();

    // how to pass a string argument:
    const char* name = args->gets(); // get the string argument
    if (name) {
        myCmdData->name = (char*)RTAlloc(inWorld, strlen(name) + 1); // allocate space, free it in cmdCleanup.
        if (!myCmdData->name) {
            Print("cmdZmqPub: memory allocation failed!\n");
            return;
        }
        strcpy(myCmdData->name, name); // copy the string
    }

    // how to pass a completion message
    int msgSize = args->getbsize();
    char* msgData = 0;
    if (msgSize) {
        // allocate space for completion message
        // scsynth will delete the completion message for you.
        msgData = (char*)RTAlloc(inWorld, msgSize);
        if (!msgData) {
            Print("cmdZmqPub: memory allocation failed!\n");
            return;
        }
        args->getb(msgData, msgSize); // copy completion message.
    }

    DoAsynchronousCommand(inWorld, replyAddr, "cmdDemoFunc", (void*)myCmdData, (AsyncStageFn)cmdStage2,
        (AsyncStageFn)cmdStage3, (AsyncStageFn)cmdStage4, cmdCleanup, msgSize, msgData);

    Print("<-cmdDemoFunc\n");
}

/*
 * to test the above, send the server these commands:
 *
 *
 * SynthDef(\sine, { Out.ar(0, SinOsc.ar(800,0,0.2)) }).load(s);
 * s.sendMsg(\cmd, \zmqPub, 7, 9, \mno, [\s_new, \sine, 900, 0, 0]);
 * s.sendMsg(\n_free, 900);
 * s.sendMsg(\cmd, \zmqPub, 7, 9, \mno);
 * s.sendMsg(\cmd, \zmqPub, 7, 9);
 * s.sendMsg(\cmd, \zmqPub, 7);
 * s.sendMsg(\cmd, \zmqPub);
 *
 */

C_LINKAGE SC_API_EXPORT void unload(InterfaceTable* inTable) 
{
    delete gThreadContainer;
}

// the entry point is called by the host when the plug-in is loaded
PluginLoad(ZmqPubUGens) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable

    // registerUnit takes the place of the Define*Unit functions. It automatically checks for the presence of a
    // destructor function.
    // However, it does not seem to be possible to disable buffer aliasing with the C++ header.
    registerUnit<ZmqPub>(ft, "ZmqPub");

    // define a plugin command - example code
    gMyPlugin.a = 1.2f;
    gMyPlugin.b = 3.4f;
    DefinePlugInCmd("zmqPub", cmdZmqPub, (void*)&gMyPlugin);
}
