### ZmqOut
A (very) experimental supercollider plugin that allows streaming SC data out via ZeroMQ sockets. It provides:
- `ZmqOut` UGen
- `zmqOut` plugin command

### Example runs:
`zmqOut` plugin command:
```
s.sendMsg(\cmd, \zmqOut, \start, 0, "tcp://*:5555");    // start a zmq PUB socket with index 0
s.sendMsg(\cmd, \zmqOut, \streamBuffer, 0, b.bufnum);   // stream b buffer via socket 0
s.sendMsg(\cmd, \zmqOut, \stop, 0);                     // stop the socket
```

`ZmqOut` UGen:
```
(
s.sendMsg(\cmd, \zmqOut, \start, 0, "tcp://*:5555");    // start a zmq PUB socket with index 0

SynthDef(\ZmqOut, {
    Out.ar(0, ZmqOut.ar(SinOsc.ar, 0));                 // stream a Sin signal to socket 0
}).add;
)

t = Synth(\ZmqOut);
t.free;
```
    

### Windows Build
Requires cmake and Visual Studio C++. 2022 community edition works fine.

Install zeromq static lib via vcpkg

```
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg.exe install zeromq:x64-windows-static
```

Optional: In order to build a specific commit, edit `vcpkg/ports/zeromq/portfile.cmake` to have:
```
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO zeromq/libzmq
    REF 8c725093ac4b44a97e6cb64566989ef12b71986c
    HEAD_REF master
    SHA512 6258c64d413a4b09d567b854df28396f6f22d7bc6efc7f6fad97c5765004d961620c6d3d64e64c3160d1ec720750cfb77097fb81152f53db99769b85e09b3371
)
```

Get supercollider sources.

Then in build folder eg: `C:\sc\example-plugins\ZmqPub\build>`
```
cmake -A x64 -DSC_PATH=C:\sc\SuperCollider "-DCMAKE_TOOLCHAIN_FILE=C:\sc\vcpkg\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static ..
cmake --build . --config Release
```
