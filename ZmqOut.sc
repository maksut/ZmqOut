// without mul and add.
ZmqOut : UGen {
    *ar { arg in = 0.0, socket = 0.0;
        ^this.multiNew('audio', in, socket)
    }
    *kr { arg in = 0.0, socket = 0.0;
        ^this.multiNew('control', in, socket)
    }
}