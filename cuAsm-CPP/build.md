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
```
```cmd
msbuild build\cuAsmCPP.sln /p:Configuration=Release
```
