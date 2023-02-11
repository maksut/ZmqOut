#include "SC_PlugIn.hpp"
#include "SC_SyncCondition.h"
#include "zmq.h"
#include <iostream>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>

// InterfaceTable contains pointers to functions in the host (server).
static InterfaceTable *ft;

#define MAX_PUB_SOCKETS 32
#define CONTROLLER_SOCKET_ADDR "ipc://c:/sc/test"
#define CHECK_ZMQ_RC(OP) if (rc < 0) { Print("%s failed with %d : %s\n", OP, rc, zmq_strerror(zmq_errno())); return; }
#define CHECK_ZMQ_RC(OP, RET) if (rc < 0) { Print("%s failed with %d : %s\n", OP, rc, zmq_strerror(zmq_errno())); return RET; }

struct TaskContext
{
    World* world;
    void* zmqContext;
    void* controllerSocket;
    void* pubSockets[MAX_PUB_SOCKETS];

    TaskContext(World* world):world(world)
    {
        memset(pubSockets, 0, sizeof(void*) * MAX_PUB_SOCKETS);
        zmqContext = zmq_ctx_new();

        controllerSocket = zmq_socket(zmqContext, ZMQ_PAIR);

        int rc = zmq_bind(controllerSocket, CONTROLLER_SOCKET_ADDR);
        CHECK_ZMQ_RC("zmq_bind in main");
    }

    ~TaskContext()
    {
        for (int i = 0; i < MAX_PUB_SOCKETS; ++i)
        {
            if (pubSockets[i])
                zmq_close(pubSockets[i]);
        }
        zmq_close(controllerSocket);
        zmq_ctx_destroy(zmqContext);
        Print("~TaskContext()\n");
    }
};

enum 
{
    UNKNOWN = 0,
    START_PUB_SOCKET = 1,
    STOP_PUB_SOCKET = 2,
    PUBLISH = 3,
};

struct Task
{
private:
    int8_t type;
    int16_t index;
    void* data;
    int dataSize;

public:
    Task(int8_t type, int16_t index, void* data, int dataSize):type(type),index(index),data(data),dataSize(dataSize){}
    Task():Task(UNKNOWN, 0, nullptr, 0){}

    void perform(TaskContext& context)
    {
        std::cout << "NRTLock" << std::endl;
        NRTLock(context.world);

        switch (type)
        {
        case START_PUB_SOCKET:
            startPubSocket(context);
            break;
        case STOP_PUB_SOCKET:
            stopPubSocket(context);
            break;
        case PUBLISH:
            publish(context);
            break;
        }

        std::cout << "RTFree taskd data" << std::endl;
        RTFree(context.world, data); // free(0) is no-op

        std::cout << "NRTUnLock" << std::endl;
        NRTUnlock(context.world);
    }

private:
    void startPubSocket(TaskContext& context)
    {
        Print("startPubSocket task\n");
        if (index < 0 || index > MAX_PUB_SOCKETS || !data || dataSize <= 0)
            return;

        // data should have our address, null terminated
        const char* address = (char*)data;
        Print("startPubSocket perform with address %s\n", address);

        // stop the existing socket if any
        if (context.pubSockets[index])
        {
            int rc = zmq_close(context.pubSockets[index]);
            CHECK_ZMQ_RC("zmq_close on stopPubSocket");
        }

        context.pubSockets[index] = zmq_socket(context.zmqContext, ZMQ_PUB);

        int rc = zmq_bind(context.pubSockets[index], address);
        CHECK_ZMQ_RC("zmq_bind in startPubSocket");

        // TODO: send a message back via controller socket
        Print("startPubSocket task end\n");
    }

    void stopPubSocket(TaskContext& context)
    {
        Print("stopPubSocket task\n");
        if (index >= MAX_PUB_SOCKETS || index < 0 || !context.pubSockets[index])
            return;

        int rc = zmq_close(context.pubSockets[index]);
        CHECK_ZMQ_RC("zmq_close on stopPubSocket");

        // TODO: send a message back via controller socket
        std::cout << "stopPubSocket task end" << std::endl;
        Print("stopPubSocket task end\n");
    }

    void publish(TaskContext& context)
    {
        Print("publish task\n");

        if (index < 0 || index > MAX_PUB_SOCKETS || !data || dataSize <= 0 || !context.pubSockets[index])
            return;

        int rc = zmq_send(context.pubSockets[index], data, dataSize, ZMQ_NOBLOCK);
        CHECK_ZMQ_RC("zmq_close on stopPubSocket");

        Print("publish task end\n");
    }
};

class WorkerThread 
{
    SC_SyncCondition mHasData;

#ifdef SUPERNOVA
    boost::lockfree::queue<Task, boost::lockfree::capacity<512>> mTaskQueue;
#else
    boost::lockfree::spsc_queue<Task, boost::lockfree::capacity<512>> mTaskQueue;
#endif

    World* mWorld;
    std::atomic<bool> mRunning;
    SC_Thread mThread;

public:
    WorkerThread(World* world) :mRunning(false), mWorld(world) {}

    ~WorkerThread() 
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

    void launchThread() 
    {
        mThread = std::thread(std::bind(&WorkerThread::ioThreadFunc, this));
    }

    bool send(Task& task) 
    {
        bool pushSucceeded = mTaskQueue.push(task);

        if (pushSucceeded)
            mHasData.Signal();

        return pushSucceeded;
    }

private:
    void ioThreadFunc() 
    {
        Print("Running the thread\n");
        mRunning.store(true);

        TaskContext context(mWorld);

        while (mRunning.load()) 
        {
            Print("Waiting for new data\n");
            mHasData.WaitEach();
            Print("Got a signal\n");

            while (1)
            {
                Task task;
                bool popSucceeded = mTaskQueue.pop(&task, 1);

                Print("popSucceeded=%d\n", popSucceeded);

                if (popSucceeded)
                    task.perform(context);
                else
                    break;
            }
        }

        Print("Stopping the thread\n");
    }
};

WorkerThread* gWorkerThread = nullptr;

// declare struct to hold unit generator state
struct ZmqPub : public SCUnit {
// Constructor usually does 3 things.
// 1. set the calculation function.
// 2. initialize the unit generator state variables.
// 3. calculate one sample of output.
public:
    ZmqPub() {
        Print("ZmqPub constructor\n");

        // test zmq pub
        Print("Sending Hello\n");

        size_t dataSize = sizeof(float) * 4;
        float* data = (float*)RTAlloc(mWorld, dataSize);
        data[0] = 0.1f;
        data[1] = 0.2f;
        data[2] = 0.3f;
        data[3] = 0.4f;

        if (gWorkerThread)
        {
            // TODO: fetch index from args
            Task task(PUBLISH, 0/*index*/, data, dataSize);
            gWorkerThread->send(task);
        }

        // 1. set the calculation function.
        if (isAudioRateIn(0)) {
            // if the frequency argument is audio rate
            set_calc_function<ZmqPub,&ZmqPub::next_a>();
        } else {    
        // if thene frequency argument is control rate (or a scalar).
            set_calc_function<ZmqPub,&ZmqPub::next_k>();
        }   

        // 2. initialize the unit generator state variables.
        // initialize a constant for multiplying the frequency
        mFreqMul = 2.0 * sampleDur();
        // get initial phase of oscillator
        mPhase = in0(1);

        // 3. calculate one sample of output.
        if (isAudioRateIn(0)) {
            next_a(1);
        } else {
            next_k(1);
        }
    }

private:
    double mPhase; // phase of the oscillator, from -1 to 1.
    float mFreqMul; // a constant for multiplying frequency
    //////////////////////////////////////////////////////////////////

    // The calculation function executes once per control period
    // which is typically 64 samples.

    // calculation function for an audio rate frequency argument
    void next_a(int inNumSamples)
    {
        // get the pointer to the output buffer
        float *outBuf = out(0);

        // get the pointer to the input buffer
        const float *freq = in(0);

        // get phase and freqmul constant from struct and store it in a
        // local variable.
        // The optimizer will cause them to be loaded it into a register.
        float freqmul = mFreqMul;
        double phase = mPhase;

        // perform a loop for the number of samples in the control period.
        // If this unit is audio rate then inNumSamples will be 64 or whatever
        // the block size is. If this unit is control rate then inNumSamples will
        // be 1.
        for (int i=0; i < inNumSamples; ++i)
        {
            // out must be written last for in place operation
            float z = phase;
            phase += freq[i] * freqmul;

            // these if statements wrap the phase a +1 or -1.
            if (phase >= 1.f) phase -= 2.f;
            else if (phase <= -1.f) phase += 2.f;

            // write the output
            outBuf[i] = z;
        }

        // store the phase back to the struct
        mPhase = phase;
    }

    //////////////////////////////////////////////////////////////////

    // calculation function for a control rate frequency argument
    void next_k(int inNumSamples)
    {
        // get the pointer to the output buffer
        float *outBuf = out(0);

        // freq is control rate, so calculate it once.
        float freq = in0(0) * mFreqMul;

        // get phase from struct and store it in a local variable.
        // The optimizer will cause it to be loaded it into a register.
        double phase = mPhase;

        // since the frequency is not changing then we can simplify the loops
        // by separating the cases of positive or negative frequencies.
        // This will make them run faster because there is less code inside the loop.
        if (freq >= 0.f) {
            // positive frequencies
            for (int i=0; i < inNumSamples; ++i)
            {
                outBuf[i] = phase;
                phase += freq;
                if (phase >= 1.f) phase -= 2.f;
            }
        } else {
            // negative frequencies
            for (int i=0; i < inNumSamples; ++i)
            {
                outBuf[i] = phase;
                phase += freq;
                if (phase <= -1.f) phase += 2.f;
            }
        }

        // store the phase back to the struct
        mPhase = phase;
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
    if (gWorkerThread == nullptr)
    {
        gWorkerThread = new WorkerThread(world);
        gWorkerThread->launchThread();
    }

    // Send a ConnectTask so our thread binds a zmq port.
    // But wait for it to complete so we can capture the context and publisher socket.

    char* address = myCmdData->name;
    Task task(START_PUB_SOCKET, 0, address, strlen(address)+1);
    gWorkerThread->send(task);

    // now the worker thread owns the address
    myCmdData->name = 0;

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

C_LINKAGE SC_API_EXPORT void unload(InterfaceTable* inTable) {
    delete gWorkerThread; 
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
