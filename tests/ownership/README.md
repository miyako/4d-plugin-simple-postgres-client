# Ownership regressions

`engine_harness.cpp` includes the real `4DPlugin-Simple-PostgreSQL-Client.cpp` and links it
with the real `support/4DPlugin-JSON.cpp`, every class in `4D Plugin API/Classes` and the
unchanged bundled `4DPluginAPI.c`. Only the 4D engine (`Call4D`) and libpq (`PQ*`) are
replaced by synthetic stubs; no database server is ever contacted. The fake engine tracks
every string, picture and object it hands out. It reports double disposal, leaks, use of
dead values and reads of uninitialised error results as named failures.

```sh
tests/ownership/run.sh . fixed                 # every test must pass
mkdir /tmp/base && git archive b8e31c426720 | tar -x -C /tmp/base
tests/ownership/run.sh /tmp/base baseline      # defect_*/hardening_* fail, control_* pass
```

Each test runs in its own process, built with ASan/UBSan. A sanitizer report or
`HARNESS ERROR` is never an expected outcome. This is macOS only: the Windows (`_WIN32`)
branches of the JSON helpers are not compiled here, and nothing is run inside a real 4D
runtime.
