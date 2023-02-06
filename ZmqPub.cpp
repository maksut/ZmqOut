#include "SC_PlugIn.hpp"
#include "SC_SyncCondition.h"
#include "zmq.h"
#include <iostream>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>

// InterfaceTable contains pointers to functions in the host (server).
static InterfaceTable *ft;

struct Task
{
    virtual ~Task() {}
    virtual void perform() = 0;
};

class WorkerThread {
    SC_SyncCondition mHasData;

#ifdef SUPERNOVA
    boost::lockfree::queue<Task*, boost::lockfree::capacity<512>> mTaskQueue;
#else
    boost::lockfree::spsc_queue<Task*, boost::lockfree::capacity<512>> mTaskQueue;
#endif

    std::atomic<bool> mRunning;
    SC_Thread mThread;

public:
    WorkerThread() :mRunning(false) {}

    ~WorkerThread() {
        if (mRunning) {
            mRunning.store(false);
            mHasData.Signal();
            mThread.join();
        }
    }

    void launchThread() {
        mThread = std::thread(std::bind(&WorkerThread::ioThreadFunc, this));
    }

    bool send(Task* task) {
        bool pushSucceeded = mTaskQueue.push(task);

        if (pushSucceeded)
            mHasData.Signal();

        return pushSucceeded;
    }

private:
    void ioThreadFunc() {
        std::cout << "Running the thread" << std::endl;
        mRunning.store(true);

        while (mRunning.load()) {
            std::cout << "Waiting for new data" << std::endl;
            mHasData.WaitEach();
            std::cout << "Got a signal" << std::endl;

            Task* task = nullptr;
            bool popSucceeded = mTaskQueue.pop(task);

            std::cout << "Pop succeeded" << popSucceeded << std::endl;

            if (popSucceeded)
                task->perform();
        }
    }
};

WorkerThread* gWorkerThread;

struct ConnectTask : public Task
{
    const char* address;
    void* publisher;
    void* context;
    SC_SyncCondition& complete;

    ConnectTask(const char* address, SC_SyncCondition& complete) :address(address), publisher(nullptr), context(nullptr), complete(complete) {}

    void perform()
    {
        std::cout << "ConnectTask perform with address " << address << std::endl;

        context = zmq_ctx_new();
        publisher = zmq_socket(context, ZMQ_PUB);

        int rc = zmq_bind(publisher, address);

        std::cout << "zmq_bind with rc " << rc << " and port " << address << std::endl;
        if (rc != 0)
            std::cout << "error: " << zmq_strerror(errno) << std::endl;

        complete.Signal();
    }
};

struct SendTask : public Task
{
private:
    struct World* mWorld;
    void* mData;
    size_t mDataSize;
    void* mPublisher;

    SendTask(struct World* world, void* publisher, void* data, size_t dataSize)
        :mWorld(world),mData(data),mDataSize(dataSize),mPublisher(publisher){}

public:
    static SendTask* Create(struct World* world, void* publisher, void* data, size_t dataSize) {
        std::cout << "Allocating a new SendTask" << std::endl;

        void* buffer = RTAlloc(world, sizeof(SendTask));
        SendTask* task = new (buffer) SendTask(world, publisher, data, dataSize);
        std::cout << "Allocated a new SendTask" << std::endl;

        return task;
    }

    void perform()
    {
        std::cout << "SendTask perform" << std::endl;

        int rc = zmq_send(mPublisher, mData, mDataSize, 0); // FLAGS?
        std::cout << "zmq_send with rc " << rc << std::endl;

        std::cout << "Message is self destructing" << std::endl;
        // Self destruct!
        struct World* world = mWorld;
        NRTLock(world);
        RTFree(world, this);
        NRTUnlock(world);
    }
};

/*
//DisconnectTask
        int rc = zmq_disconnect(publisher, mZmqAddress);
        std::cout << "zmq_disconnect with rc " << rc << std::endl;
*/

// zmq context is thread safe
void* gZmqContext;

// But this socket handle is NOT thread safe. It needs to be created and used in worker thread
void* gZmqPublisher;

// declare struct to hold unit generator state
struct ZmqPub : public SCUnit {
private:
    float* mData;

// Constructor usually does 3 things.
// 1. set the calculation function.
// 2. initialize the unit generator state variables.
// 3. calculate one sample of output.
public:
    ZmqPub() {
        std::cout << "ZmqPub constructor" << std::endl;

        // test zmq pub
        std::cout << "Sending Hello" << std::endl;

        size_t dataSize = sizeof(float) * 4;
        mData = (float*)RTAlloc(mWorld, dataSize);
        mData[0] = 0.1f;
        mData[1] = 0.2f;
        mData[2] = 0.3f;
        mData[3] = 0.4f;

        auto task = SendTask::Create(mWorld, gZmqPublisher, mData, dataSize);

        gWorkerThread->send(task);

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

    ~ZmqPub()
    {
        RTFree(mWorld, mData);
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

C_LINKAGE SC_API_EXPORT void unload(InterfaceTable* inTable) {
    zmq_ctx_destroy(gZmqContext);
    delete gWorkerThread; 
}

// the entry point is called by the host when the plug-in is loaded
PluginLoad(ZmqPubUGens) {
    std::cout << "ZmqPub plugin load" << std::endl;

    // DiskIO_UGens does a similar allocation on heap during plugin load. So it must be OK.
    gWorkerThread = new WorkerThread();
    gWorkerThread->launchThread();

    // Send a ConnectTask so our thread binds a zmq port.
    // But wait for it to complete so we can capture the context and publisher socket.
    SC_SyncCondition complete;
    ConnectTask connect = ConnectTask("tcp://*:5555", complete);
    gWorkerThread->send(&connect);

    std::cout << "Waiting for connection to complete" << std::endl;
    complete.WaitEach(); // wait for connection to complete
    std::cout << "Connection is complete" << std::endl;

    gZmqContext = connect.context;
    gZmqPublisher = connect.publisher;

    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable

    // registerUnit takes the place of the Define*Unit functions. It automatically checks for the presence of a
    // destructor function.
    // However, it does not seem to be possible to disable buffer aliasing with the C++ header.
    registerUnit<ZmqPub>(ft, "ZmqPub");
}
