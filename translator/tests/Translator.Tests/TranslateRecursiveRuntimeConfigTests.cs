using System.Buffers.Binary;
using System.Reflection;
using System.Threading.Tasks;
using Translator.Cli.Configuration;
using Translator.Core.Mods;
using Xunit;

namespace Translator.Tests;

public sealed class TranslateRecursiveRuntimeConfigTests
{
    [Fact]
    public void SpeculativeRootConstructorStoreCanSeedNullHeaderVTableMethods()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-speculative-vptr-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            const uint entry = 0x80001000u;
            const uint mapRoot = 0x80001100u;
            const uint constructor = 0x80001200u;
            const uint methodsText = 0x80001300u;
            const uint methodA = methodsText + 0x04u;
            const uint methodB = methodsText + 0x0Cu;
            const uint methodC = methodsText + 0x14u;
            const uint table = 0x80002000u;

            var tableBytes = new byte[0x20];
            BinaryPrimitives.WriteUInt32BigEndian(tableBytes.AsSpan(0x08, 4), methodA);
            BinaryPrimitives.WriteUInt32BigEndian(tableBytes.AsSpan(0x0C, 4), methodB);
            BinaryPrimitives.WriteUInt32BigEndian(tableBytes.AsSpan(0x10, 4), methodC);
            BinaryPrimitives.WriteUInt32BigEndian(tableBytes.AsSpan(0x14, 4), 0x12345678u);

            File.WriteAllBytes(
                Path.Combine(root, "main.dol"),
                SyntheticDolFactory.CreateBytes(
                    entry,
                    sections:
                    [
                        SyntheticDolFactory.Text(0, entry, 0x4E800020u), // blr
                        SyntheticDolFactory.Text(
                            1,
                            mapRoot,
                            0x48000101u, // bl constructor
                            0x4E800020u), // blr
                        SyntheticDolFactory.Text(
                            2,
                            constructor,
                            0x3CA08000u, // lis r5,0x8000
                            0x38A52000u, // addi r5,r5,0x2000 => table
                            0x90A30000u, // stw r5,0(r3)
                            0x4E800020u), // blr
                        SyntheticDolFactory.Text(
                            3,
                            methodsText,
                            0x4E800020u, // raw terminal boundary before method A
                            0x38600001u, // li r3,1
                            0x4E800020u,
                            0x38600002u, // method B: li r3,2
                            0x4E800020u,
                            0x38600003u, // method C: li r3,3
                            0x4E800020u),
                        SyntheticDolFactory.Data(0, table, tableBytes)
                    ]));
            File.WriteAllText(Path.Combine(root, "function-map.txt"), $"{mapRoot:X8} speculative_map_root\n");

            var projectPath = Path.Combine(root, "recomp.yml");
            File.WriteAllText(
                projectPath,
                """
                schema_version: 1
                project:
                  id: speculative-vptr-test
                memory:
                  sda_base: 0x80003000
                  sda2_base: 0x80004000
                inputs:
                  dol:
                    path: main.dol
                translation:
                  entry_points: [0x80001000]
                  function_map:
                    path: function-map.txt
                  discover_static_callbacks: true
                output:
                  root: generated
                """);

            var outputDir = Path.Combine(root, "generated", "functions");
            var metadataPath = Path.Combine(root, "generated", "metadata.json");
            Assert.Equal(
                0,
                InvokeCli(
                    "translate-recursive",
                    $"0x{entry:X8}",
                    "--project", projectPath,
                    "--outdir", outputDir,
                    "--output-metadata", metadataPath,
                    "--threads", "1"));

            var metadata = BaseTranslationOutputMetadataFile.Read(metadataPath);
            var translated = metadata.Functions.Select(static function => function.EntryPoint).ToHashSet();
            Assert.Contains(mapRoot, translated);
            Assert.Contains(constructor, translated);
            Assert.Contains(methodA, translated);
            Assert.Contains(methodB, translated);
            Assert.Contains(methodC, translated);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void DenseCallbackTableBootstrapsFromTranslatedReturnBoundaryAcrossWaves()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-dense-callback-bootstrap-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            const uint entry = 0x80001000u;
            const uint callbackB = 0x80001008u;
            const uint callbackC = 0x80001010u;
            const uint table = 0x80002000u;

            var tableBytes = new byte[0x10];
            BinaryPrimitives.WriteUInt32BigEndian(tableBytes.AsSpan(0x00, 4), callbackB);
            BinaryPrimitives.WriteUInt32BigEndian(tableBytes.AsSpan(0x04, 4), callbackC);
            BinaryPrimitives.WriteUInt32BigEndian(tableBytes.AsSpan(0x08, 4), 0x12345678u);

            File.WriteAllBytes(
                Path.Combine(root, "main.dol"),
                SyntheticDolFactory.CreateBytes(
                    entry,
                    sections:
                    [
                        SyntheticDolFactory.Text(
                            0,
                            entry,
                            0x38600000u, // entry: li r3,0
                            0x4E800020u, // blr; exclusive end is callbackB
                            0x38600001u, // callbackB: li r3,1
                            0x4E800020u, // blr; exclusive end is callbackC
                            0x38600002u, // callbackC: li r3,2
                            0x4E800020u),
                        SyntheticDolFactory.Data(0, table, tableBytes)
                    ]));

            var projectPath = Path.Combine(root, "recomp.yml");
            File.WriteAllText(
                projectPath,
                """
                schema_version: 1
                project:
                  id: dense-callback-bootstrap-test
                memory:
                  sda_base: 0x80003000
                  sda2_base: 0x80004000
                inputs:
                  dol:
                    path: main.dol
                translation:
                  entry_points: [0x80001000]
                  discover_static_callbacks: true
                output:
                  root: generated
                """);

            var outputDir = Path.Combine(root, "generated", "functions");
            var metadataPath = Path.Combine(root, "generated", "metadata.json");
            Assert.Equal(
                0,
                InvokeCli(
                    "translate-recursive",
                    $"0x{entry:X8}",
                    "--project", projectPath,
                    "--outdir", outputDir,
                    "--output-metadata", metadataPath,
                    "--threads", "1"));

            var metadata = BaseTranslationOutputMetadataFile.Read(metadataPath);
            var translated = metadata.Functions.Select(static function => function.EntryPoint).ToHashSet();
            Assert.Equal(3, translated.Count);
            Assert.Contains(entry, translated);
            Assert.Contains(callbackB, translated);
            Assert.Contains(callbackC, translated);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void UnsupportedOpcodeFailsStrictTranslation()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-unsupported-opcode-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            const uint entry = 0x80001000u;
            File.WriteAllBytes(
                Path.Combine(root, "main.dol"),
                SyntheticDolFactory.CreateBytes(
                    entry,
                    sections: [SyntheticDolFactory.Text(
                        0,
                        entry,
                        0x04000000u, // unassigned primary opcode; deliberately unsupported by the lifter
                        0x4E800020u)])); // blr
            var projectPath = Path.Combine(root, "recomp.yml");
            File.WriteAllText(
                projectPath,
                """
                schema_version: 1
                project:
                  id: unsupported-opcode-test
                memory:
                  sda_base: 0x80002000
                  sda2_base: 0x80003000
                inputs:
                  dol:
                    path: main.dol
                translation:
                  entry_points: [0x80001000]
                output:
                  root: generated
                """);
            Assert.False(TranslationProjectConfig.Load(projectPath).Translation.AllowUnsupportedInstructions);

            var strictError = Assert.Throws<TargetInvocationException>(() => InvokeCli(
                "translate-recursive",
                $"0x{entry:X8}",
                "--project", projectPath,
                "--outdir", Path.Combine(root, "strict", "functions"),
                "--output-metadata", Path.Combine(root, "strict", "metadata.json"),
                "--threads", "1"));
            Assert.Contains("UNIMPLEMENTED", strictError.InnerException?.ToString(), StringComparison.Ordinal);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void RepeatedRunsReuseUnchangedOutputsAndPruneStaleFiles()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-recursive-prune-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            const uint entry = 0x80001000u;
            File.WriteAllBytes(
                Path.Combine(root, "main.dol"),
                SyntheticDolFactory.CreateBytes(
                    entry,
                    sections: [SyntheticDolFactory.Text(
                        0,
                        entry,
                        0x60000000u, // nop
                        0x60000000u, // nop
                        0x4E800020u)])); // blr
            var projectPath = Path.Combine(root, "recomp.yml");
            File.WriteAllText(
                projectPath,
                """
                schema_version: 1
                project:
                  id: recursive-prune-test
                  display_name: Recursive Prune Test
                memory:
                  sda_base: 0x80002000
                  sda2_base: 0x80003000
                inputs:
                  dol:
                    path: main.dol
                translation:
                  entry_points: [0x80001000]
                output:
                  root: generated
                """);

            var stagedFunctions = Path.Combine(root, "staging", "functions");
            var stagedOutputMetadata = Path.Combine(root, "staging", "functions_metadata.json");
            var exitCode = InvokeCli(
                "translate-recursive",
                $"0x{entry:X8}",
                "--project",
                projectPath,
                "--outdir",
                stagedFunctions,
                "--output-metadata",
                stagedOutputMetadata,
                "--threads",
                "1");

            Assert.Equal(0, exitCode);
            var firstMetadata = BaseTranslationOutputMetadataFile.Read(stagedOutputMetadata);
            Assert.Null(firstMetadata.TranslationIdentityHash);
            Assert.Equal(0, firstMetadata.Quality.UnsupportedInstructionCount);
            Assert.Equal(0, firstMetadata.Quality.InvalidSsaFunctionCount);
            Assert.Single(firstMetadata.Functions);
            Assert.Single(Directory.GetFiles(stagedFunctions, "*.cpp"));
            var generatedPath = Directory.GetFiles(stagedFunctions, "*.cpp").Single();
            var unchangedTimestamp = new DateTime(2004, 5, 6, 7, 8, 10, DateTimeKind.Utc);
            File.SetLastWriteTimeUtc(generatedPath, unchangedTimestamp);
            // A populated output tree is not an input to recursive discovery.
            // This stale filename used to become a synthetic function boundary
            // and truncate the second translation at entry + 4.
            File.WriteAllText(Path.Combine(stagedFunctions, $"func_{entry + 4:X8}.cpp"), "stale output hint");
            var metadataTimestamp = new DateTime(2004, 5, 6, 7, 8, 12, DateTimeKind.Utc);
            File.SetLastWriteTimeUtc(stagedOutputMetadata, metadataTimestamp);

            var secondExitCode = InvokeCli(
                "translate-recursive",
                $"0x{entry:X8}",
                "--project",
                projectPath,
                "--outdir",
                stagedFunctions,
                "--output-metadata",
                stagedOutputMetadata,
                "--threads",
                "1");

            Assert.Equal(0, secondExitCode);
            Assert.Equal(unchangedTimestamp, File.GetLastWriteTimeUtc(generatedPath));
            Assert.Equal(metadataTimestamp, File.GetLastWriteTimeUtc(stagedOutputMetadata));

            var stalePath = Path.Combine(stagedFunctions, "func_80001004.cpp");
            var unlistedPath = Path.Combine(stagedFunctions, "unlisted.cpp");
            File.WriteAllText(stalePath, "// stale");
            File.WriteAllText(unlistedPath, "// not translator-owned");
            BaseTranslationOutputMetadataFile.WriteIfChangedAtomic(
                stagedOutputMetadata,
                BaseTranslationOutputMetadata.Create(
                    firstMetadata.Functions.Concat([
                        new BaseTranslationFunctionMetadata(
                            "func_80001004.cpp",
                            8,
                            new string('e', 64),
                            0x80001004u,
                            [])
                    ]),
                    TranslationQualityMetadata.Clean));

            var pruneExitCode = InvokeCli(
                "translate-recursive",
                $"0x{entry:X8}",
                "--project",
                projectPath,
                "--outdir",
                stagedFunctions,
                "--output-metadata",
                stagedOutputMetadata,
                "--prune-stale",
                "--threads",
                "1");
            Assert.Equal(0, pruneExitCode);
            Assert.False(File.Exists(stalePath));
            Assert.True(File.Exists(unlistedPath));
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    private static int InvokeCli(params string[] args)
    {
        var entryPoint = typeof(TranslationProjectConfig).Assembly.EntryPoint;
        Assert.NotNull(entryPoint);
        var result = entryPoint!.Invoke(null, new object?[] { args });
        return result switch
        {
            int exitCode => exitCode,
            Task<int> intTask => intTask.GetAwaiter().GetResult(),
            Task task => CompleteTask(task),
            null => 0,
            _ => throw new InvalidOperationException($"Unexpected CLI entry point result type: {result.GetType().FullName}")
        };
    }

    private static int CompleteTask(Task task)
    {
        task.GetAwaiter().GetResult();
        return 0;
    }
}
