Modernize Pigz --> Pigz++

pigz, which stands for Parallel Implementation of GZip, is a fully functional replacement for gzip that exploits multiple processors and multiple cores when compressing data. This version of pigz is written to be portable across Unix-style operating systems that provide the zlib and pthread libraries.

Goals:
* Modernize pigz by rewriting it in C++ -- call it pigzpp. The original pigz was written when programming languages were still evolving, and some utilities were hand-written at that time. Now we can use C++23 and leverage language and standard library features such as try-catch, threads, etc.

* CRITICAL: The modernized pigz must be at least as fast as the original pigz. Add benchmarks to ensure performance parity. Pay attention to the original code being replaced -- some utilities may be hand-written for speed or efficiency in ways that outperform standard library equivalents. Preserve these optimizations and modernize them minimally without degrading performance.

* CRITICAL: Compatibility. Pigz is designed to be compatible with gzip -- you can compress with pigz and decompress with gzip, or vice versa. We must maintain that compatibility.

* CRITICAL: Separate code into library and executable. The original pigz was a standalone CLI tool. We want to create a library that can be embedded in other C++ and Python programs. The original code uses a single global namespace; we need thread-safe library functions that work across multiple threads in the same process. For example: coroutines with one thread reading via pigz and another writing via pigz, or multiple application threads reading and writing different files simultaneously.

* Create an executable using the pigz library -- call it pigzpp. It must have the same or similar CLI interface and behavior as pigz. Add tests to ensure command-line compatibility and behavior.

* Use CMake as the build system and CTest as the testing framework. Consider GoogleTest for complex test suites.

* Python bindings exist and can be deferred to Stage 2. Stage 1 focuses on the C++ rewrite, tests, and benchmarks. Proceed to Stage 2 only if Stage 1 achieves performance parity with the original pigz.
