# Book hook forwarding regression

Build and run independently of the plugin and its game dependencies:

```powershell
cmake -S tests/book-framework -B build/book-framework-tests -A x64
cmake --build build/book-framework-tests --config Release
ctest --test-dir build/book-framework-tests -C Release --output-on-failure
```

The test compiles the production `BookFramework.cpp` with small game-service
and SafetyHook doubles. The original-function double invokes a real C++
function pointer using the explicit types supplied by the production hook.
The engine functions are not inlined, so an x64 Windows build exercises the
register and stack argument boundary, including VR's ninth argument.

One hundred eight cases cover runtime hook selection, null/non-null VR scene
pointers, world/inventory reference arguments, both position flags,
unchanged text, replacement text, empty replacement, replacement failure,
null books, a logging exception after replacement construction, and stored
text lengths of 65534, 65535 and 65536 bytes. They
check all arguments, reference identity, replacement lifetime and exactly
one original invocation. Construction counters reject unnecessary default
game strings, move assignment and retained temporary strings. Compile-time
checks enforce the distinct native signatures.

These checks do not install an actual SafetyHook detour, allocate engine
objects, or validate in-game book rendering and object lifetime. Those
require a rebuilt DLL and a game session.
