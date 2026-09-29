using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Translator.Core.IO;
using Translator.Core.Parsing.Rso;
using Translator.Core.Translation;

// Synthetic header, one PPC blr and deterministic data: no game inputs needed.
var mebibytes = args.Length > 0 ? int.Parse(args[0]) : 4;
if (mebibytes is < 1 or > 32) throw new ArgumentOutOfRangeException(nameof(mebibytes));
var bytes = new byte[mebibytes * 1024 * 1024];
for (var i = 0x200; i < bytes.Length; ++i) bytes[i] = (byte)(i * 73 + (i >> 8));
void U32(int offset, uint value) => BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(offset, 4), value);
U32(0x08, 2); U32(0x0C, 0x58); U32(0x10, 0x80); U32(0x14, 14); U32(0x18, 1);
U32(0x60, 0xA0); U32(0x64, 4); U32(0xA0, 0x4E800020); // blr
Encoding.ASCII.GetBytes("synthetic.rso\0").CopyTo(bytes, 0x80);
var file = RsoFile.Parse(bytes);
var compilation = RsoDynamicTemplateCompiler.Compile(file, new EmptySymbols(),
    new RsoDynamicTemplateCompileOptions("benchmark", 0x70000000,
        new HashSet<int> { 1 }, AdditionalFunctionOffsets: new HashSet<uint> { 0xA0 }));

// Warm JIT paths before measuring. Hashing and output I/O are outside emission timing.
_ = RsoDynamicTemplateSourceWriter.WriteRegistrarSource(file, compilation);
var results = new List<object>();
string? expectedHash = null;
var output = Path.Combine(Path.GetTempPath(), "wiicompiled-benchmark-" + Guid.NewGuid().ToString("N"));
try
{
    for (var iteration = 0; iteration < 3; ++iteration)
    {
        GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
        var allocated = GC.GetTotalAllocatedBytes(true);
        var watch = Stopwatch.StartNew();
        var source = RsoDynamicTemplateSourceWriter.WriteRegistrarSource(file, compilation);
        watch.Stop();
        var allocationBytes = GC.GetTotalAllocatedBytes(true) - allocated;
        var sourceBytes = Encoding.UTF8.GetBytes(source);
        var hash = Convert.ToHexString(SHA256.HashData(sourceBytes));
        expectedHash ??= hash;
        if (hash != expectedHash) throw new InvalidOperationException("Nondeterministic registrar");
        var updated = FileOutput.WriteBytesIfChanged(output, sourceBytes);
        var stamp = File.GetLastWriteTimeUtc(output);
        var repeat = Stopwatch.StartNew();
        var unchanged = !FileOutput.WriteBytesIfChanged(output, sourceBytes);
        repeat.Stop();
        if (!unchanged || File.GetLastWriteTimeUtc(output) != stamp)
            throw new InvalidOperationException("Identical output was rewritten");
        results.Add(new { iteration, emissionMs = watch.Elapsed.TotalMilliseconds,
            allocatedBytes = allocationBytes, outputBytes = sourceBytes.Length,
            sha256 = hash, updated, unchangedWriteMs = repeat.Elapsed.TotalMilliseconds });
    }
}
finally { if (File.Exists(output)) File.Delete(output); }
Console.WriteLine(JsonSerializer.Serialize(new { inputMiB = mebibytes, results },
    new JsonSerializerOptions { WriteIndented = true }));

sealed class EmptySymbols : IRsoSymbolProvider
{
    public bool TryResolve(string name, out uint address) { address = 0; return false; }
}
