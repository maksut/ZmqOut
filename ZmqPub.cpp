#include "SC_PlugIn.hpp"
#include "SC_SyncCondition.h"
#include "zmq.h"
#include <iostream>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>

// InterfaceTable contains pointers to functions in the host (server).
static InterfaceTable *ft;

struct Msg
{
    struct World* world;
    float* data;
    size_t dataSize;
};

class ZmqThread {

    void* mContext;

    SC_SyncCondition mZmkFifoHasData;

#ifdef SUPERNOVA
    boost::lockfree::queue<Msg, boost::lockfree::capacity<256>> mZmqFifo;
#else
    boost::lockfree::spsc_queue<Msg, boost::lockfree::capacity<256>> mZmqFifo;
#endif

    std::atomic<bool> mRunning;
    std::atomic<const char*> mZmqAddress;

    SC_Thread mThread;

public:
    ZmqThread() :mRunning(false), mContext(nullptr), mZmqAddress(nullptr) {}

    ~ZmqThread() {
        if (mRunning) {
            mRunning.store(false);
            mZmkFifoHasData.Signal();
            mThread.join();
        }
    }

    void launchThread() {
        using namespace std;
        mRunning.store(true);

        mThread = thread(bind(&ZmqThread::ioThreadFunc, this));
    }

    void connect(const char* zmqAddress)
    {
        mZmqAddress.store(zmqAddress);
        mZmkFifoHasData.Signal();
    }

    bool send(Msg& msg) {
        bool pushSucceeded = mZmqFifo.push(msg);

        if (pushSucceeded)
            mZmkFifoHasData.Signal();

        return pushSucceeded;
    }

private:
    void ioThreadFunc() {
        std::cout << "Running the thread" << std::endl;

        void* publisher = nullptr;

        while (mRunning.load()) {
            mZmkFifoHasData.WaitEach();

            if (publisher == nullptr)
                publisher = zmqBind();

            if (publisher == nullptr)
            {
                std::cout << "Oh no. Couldn't bind zmq";
                continue;
            }

            Msg msg;
            bool popSucceeded = mZmqFifo.pop(msg);

            if (popSucceeded)
                zmqSend(publisher, msg);
        }

        int rc = zmq_disconnect(publisher, mZmqAddress);
        std::cout << "zmq_disconnect with rc " << rc << std::endl;
    }

    void* zmqBind() {
        std::cout << "BindAction perform with address " << mZmqAddress << std::endl;

        mContext = zmq_ctx_new();
        void* publisher = zmq_socket(mContext, ZMQ_PUB);

        int rc = zmq_bind(publisher, mZmqAddress);

        std::cout << "zmq_bind with rc " << rc << " and port " << mZmqAddress << std::endl;

        return (rc == 0) ? publisher : nullptr;
    }

    void zmqSend(void* publisher, Msg& msg)
    {
        std::cout << "SendAction perform" << std::endl;

        int rc = zmq_send(publisher, msg.data, msg.dataSize, 0); // FLAGS?
        std::cout << "zmq_send with rc " << rc << std::endl;
    }
};

ZmqThread* gZmqIOThread;


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

        Msg msg;
        msg.world = mWorld;
        msg.data = mData;
        msg.dataSize = dataSize;
        gZmqIOThread->send(msg);

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

C_LINKAGE SC_API_EXPORT void unload(InterfaceTable* inTable) { delete gZmqIOThread; }

// the entry point is called by the host when the plug-in is loaded
PluginLoad(ZmqPubUGens)
{
    std::cout << "ZmqPub plugin load" << std::endl;

    gZmqIOThread = new ZmqThread();
    gZmqIOThread->launchThread();
    gZmqIOThread->connect("tcp://127.0.0.1:5555");

    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable

    // registerUnit takes the place of the Define*Unit functions. It automatically checks for the presence of a
    // destructor function.
    // However, it does not seem to be possible to disable buffer aliasing with the C++ header.
    registerUnit<ZmqPub>(ft, "ZmqPub");
}
