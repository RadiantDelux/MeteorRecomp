# Translator performance checks

Run `dotnet run --project benchmarks/Translator.Benchmarks -c Release -- 4`.
The argument is synthetic RSO size in MiB (1–32). The benchmark warms the JIT,
runs three emissions, reports elapsed time/allocation and hashes the complete
generated source. It also verifies that writing identical output preserves its
timestamp. No game data is used.

On this Windows machine with .NET 8, the 4 MiB registrar benchmark changed from
**208.65 ms to 70.59 ms** (median of three runs, approximately **2.96x faster**)
after replacing per-byte hexadecimal formatting with direct digit lookup.
Both produced SHA-256
`D54E2475D9F2551B3C7661A4D7C8895E45131FED2B2A1256C733CE130EDF3741`.
Allocation was essentially unchanged (136.43 vs 136.42 MiB).

This measures registrar emission, not an entire game translation or native
build. RSO function sources, signatures, images, metadata and guest symbol
tables now use the existing atomic content-aware writer, avoiding unnecessary
native rebuilds when their bytes have not changed.
