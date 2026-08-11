# For Windows
## Prerequisites
* Visual Studio 2022 including C++ build tools
* NVCC
* Boost
* Graphviz/DOT

## Build instructions
In the developer command prompt:
```cmd
cmake -S cuAsm-CPP -B build -G "Visual Studio 17 2022" -A x64
msbuild build\cuAsmCPP.sln /p:Configuration=Release
```

### Run Tests
Following the build instructions above:
```cmd
cd build
ctest -C Release --output-on-failure
```
