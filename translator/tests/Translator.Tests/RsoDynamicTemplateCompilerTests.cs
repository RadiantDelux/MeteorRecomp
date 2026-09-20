using System.Buffers.Binary;
using System.Text;
using Translator.Core.Parsing.Rso;
using Translator.Core.Translation;
using Xunit;

namespace Translator.Tests;

public class RsoDynamicTemplateCompilerTests
{
    [Fact]
    public void CompilerDiscoversInternalCallAndKeepsExternalCallFixed()
    {
        var file = RsoFile.Parse(BuildRso());
        var compilation = RsoDynamicTemplateCompiler.Compile(
            file,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions(
                "synthetic-rso",
                0x7F000000u,
                new HashSet<int> { 1 }));

        Assert.Equal(2, compilation.Functions.Count);
        Assert.Equal(new uint[] { 0xA0u, 0xACu }, compilation.Functions.Select(static f => f.Offset).ToArray());
        var main = compilation.Functions[0].Translation.CxxCode;
        Assert.Contains("InvokeCurrentDynamicModuleFunction(0x000000ACu, ctx);", main, StringComparison.Ordinal);
        Assert.Contains("InvokeDirectCpu<0x80500000u>(ctx);", main, StringComparison.Ordinal);
        Assert.DoesNotContain("RECOMP_REGISTRATION base", main, StringComparison.Ordinal);
        Assert.Equal(0x7F000200u, compilation.TemplateContext.CanonicalBssBase);
        Assert.Equal(0x10u, compilation.TemplateContext.BssSize);
    }

    [Fact]
    public void CompilerRejectsCanonicalBaseThatBreaksExternalRel24()
    {
        var file = RsoFile.Parse(BuildRso());
        Assert.Throws<InvalidDataException>(() => RsoDynamicTemplateCompiler.Compile(
            file,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions(
                "far-rso",
                0x70000000u,
                new HashSet<int> { 1 })));
    }

    [Fact]
    public void CompilerRejectsCanonicalImageInsideGuestRam()
    {
        var file = RsoFile.Parse(BuildRso());
        Assert.Throws<InvalidDataException>(() => RsoDynamicTemplateCompiler.Compile(
            file,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions(
                "bad-rso",
                0x81000000u,
                new HashSet<int> { 1 })));
    }

    [Theory]
    [InlineData(RsoRelocationType.R_PPC_ADDR24, 0xFC000003u)]
    [InlineData(RsoRelocationType.R_PPC_REL24, 0xFC000003u)]
    [InlineData(RsoRelocationType.R_PPC_ADDR14, 0xFFFF0003u)]
    [InlineData(RsoRelocationType.R_PPC_ADDR14_BRTAKEN, 0xFFFF0003u)]
    [InlineData(RsoRelocationType.R_PPC_ADDR14_BRNTAKEN, 0xFFFF0003u)]
    [InlineData(RsoRelocationType.R_PPC_REL14, 0xFFFF0003u)]
    [InlineData(RsoRelocationType.R_PPC_REL14_BRTAKEN, 0xFFFF0003u)]
    [InlineData(RsoRelocationType.R_PPC_REL14_BRNTAKEN, 0xFFFF0003u)]
    public void SignaturePreservesNonRelocatedBranchBits(RsoRelocationType type, uint invariantBits)
    {
        var bytes = BuildRso();
        WriteU32(bytes, 0x104, (1u << 8) | (uint)type);
        var file = RsoFile.Parse(bytes);
        var linked = RsoImageBuilder.Build(file, 0x7F000000u, 0x7F000200u,
            new MapProvider(("Ext", 0x80500000u)));
        var signature = RsoDynamicTemplateSourceWriter.BuildInvariantSignature(file, linked);

        Assert.Equal(invariantBits, BinaryPrimitives.ReadUInt32BigEndian(signature.CompareMask.AsSpan(0xA0, 4)));
        Assert.True(SignatureMatches(signature, linked.Image));

        // Every invariant bit must reject a changed instruction, including the
        // opcode, branch AA/LK and conditional BO/BI fields.
        var original = BinaryPrimitives.ReadUInt32BigEndian(linked.Image.AsSpan(0xA0, 4));
        for (var bit = 0; bit < 32; ++bit)
        {
            var changed = linked.Image.ToArray();
            var bitMask = 1u << bit;
            WriteU32(changed, 0xA0, original ^ bitMask);
            Assert.Equal((invariantBits & bitMask) == 0, SignatureMatches(signature, changed));
        }
    }

    [Fact]
    public void SignatureStillAcceptsLegalRelinkingAtAnotherBase()
    {
        var file = RsoFile.Parse(BuildRso());
        var provider = new MapProvider(("Ext", 0x80500000u));
        var canonical = RsoImageBuilder.Build(file, 0x7F000000u, 0x7F000200u, provider);
        var runtime = RsoImageBuilder.Build(file, 0x81000000u, 0x81200000u, provider);
        var signature = RsoDynamicTemplateSourceWriter.BuildInvariantSignature(file, canonical);

        Assert.True(SignatureMatches(signature, runtime.Image));
        Assert.Equal(0xFC000003u,
            BinaryPrimitives.ReadUInt32BigEndian(signature.CompareMask.AsSpan(0xA4, 4)));
        Assert.Equal(uint.MaxValue,
            BinaryPrimitives.ReadUInt32BigEndian(signature.CompareMask.AsSpan(0xAC, 4)));
    }

    [Fact]
    public void SignatureMasksRetailLinkerHeaderStateAndPreservedImportRelocations()
    {
        var file = RsoFile.Parse(BuildRso());
        var canonical = RsoImageBuilder.Build(
            file,
            0x7F000000u,
            0x7F000200u,
            new MapProvider(),
            new RsoImageBuildOptions { PreserveUnresolvedImports = true });
        var signature = RsoDynamicTemplateSourceWriter.BuildInvariantSignature(file, canonical);

        foreach (var offset in new[] { 0x00, 0x04, 0x34, 0x3C, 0x50 })
            Assert.Equal(0u, BinaryPrimitives.ReadUInt32BigEndian(signature.CompareMask.AsSpan(offset, 4)));

        // The external branch was intentionally unresolved by the analysis
        // link, but retail can still resolve and patch it. Preserve opcode and
        // AA/LK while masking exactly the REL24 displacement bits.
        Assert.Equal(0xFC000003u,
            BinaryPrimitives.ReadUInt32BigEndian(signature.CompareMask.AsSpan(0xA4, 4)));
    }

    [Fact]
    public void RegistrarComparesSignatureBitsRatherThanBytePresence()
    {
        var file = RsoFile.Parse(BuildRso());
        var compilation = RsoDynamicTemplateCompiler.Compile(file,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions("signature-bits", 0x7F000000u, new HashSet<int> { 1 }));
        var source = RsoDynamicTemplateSourceWriter.WriteRegistrarSource(file, compilation);

        Assert.Contains("DynamicModule::MatchMaskedImage(image, kExpected_signature_bits, kCompareMask_signature_bits, kFullImageSize)", source);
        Assert.Contains("&ValidateRetained_signature_bits", source);
        Assert.Contains("&ValidateDispatch_signature_bits", source);
        Assert.Contains("MemoryInline::ResolveDeferredReads(base, kFullImageSize)", source);
        Assert.DoesNotContain("kCompareMask_signature_bits[i] != 0 &&", source);
        Assert.Contains("const uint8_t kExpected_signature_bits[] =", source, StringComparison.Ordinal);
        Assert.Contains("\"\\x", source, StringComparison.Ordinal);
        Assert.DoesNotContain("0x48,", source, StringComparison.Ordinal);
    }

    [Fact]
    public void RegistrarCanEmbedSignatureFromBinaryFiles()
    {
        var raw = BuildRso();
        var file = RsoFile.Parse(raw);
        var compilation = RsoDynamicTemplateCompiler.Compile(
            file,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions(
                "incbin-signature",
                0x7F000000u,
                new HashSet<int> { 1 },
                AdditionalFunctionOffsets: new HashSet<uint> { 0xA0u }));

        var source = RsoDynamicTemplateSourceWriter.WriteRegistrarSource(
            file,
            compilation,
            Path.Combine(Path.GetTempPath(), "expected.bin"),
            Path.Combine(Path.GetTempPath(), "mask.bin"));

        Assert.Contains(Path.GetFullPath(Path.Combine(Path.GetTempPath(), "expected.bin")).Replace('\\', '/'), source, StringComparison.Ordinal);
        Assert.Contains(Path.GetFullPath(Path.Combine(Path.GetTempPath(), "mask.bin")).Replace('\\', '/'), source, StringComparison.Ordinal);
        Assert.DoesNotContain("const uint8_t kExpected_incbin_signature[] =\r\n    \"\\x", source, StringComparison.Ordinal);
    }

    [Fact]
    public void RegistrarPublicationOffsetsUseTerminalRelocationCacheLine()
    {
        var raw = BuildRso();
        // Put the terminal external relocation on a cache line distinct from
        // the executable section. Retail processes this record last, so this is
        // the only relocation-derived discovery probe the registrar needs.
        WriteU32(raw, 0x10C, 0x1F4u);
        var file = RsoFile.Parse(raw);
        var compilation = RsoDynamicTemplateCompiler.Compile(file,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions("publication-lines", 0x7F000000u, new HashSet<int> { 1 }));

        Assert.Equal(
            new uint[] { 0u, 0xA0u, 0x1E0u },
            RsoDynamicTemplateSourceWriter.BuildPublicationOffsets(file, compilation));

        var source = RsoDynamicTemplateSourceWriter.WriteRegistrarSource(file, compilation);
        Assert.Contains("0x000001E0u,", source, StringComparison.Ordinal);
        Assert.Contains("for (const uint32_t offset : kPublicationOffsets_publication_lines)", source, StringComparison.Ordinal);
        Assert.Contains("const uint32_t base = probe - offset;", source, StringComparison.Ordinal);
        Assert.Contains("if (!QuickMatch_publication_lines(base)) continue;", source, StringComparison.Ordinal);
        Assert.Contains("Match_publication_lines(base, &candidate)", source, StringComparison.Ordinal);
    }

    [Fact]
    public void RegistrarPrefiltersPublicationCandidatesWithMandatoryLinkedHeaderPointers()
    {
        var file = RsoFile.Parse(BuildRso());
        var compilation = RsoDynamicTemplateCompiler.Compile(file,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions("quick-match", 0x7F000000u, new HashSet<int> { 1 }));
        var source = RsoDynamicTemplateSourceWriter.WriteRegistrarSource(file, compilation);

        Assert.Contains("bool QuickMatch_quick_match(uint32_t base)", source, StringComparison.Ordinal);
        Assert.Contains("Memory::Read32(base + 0x0Cu) != base + 0x00000058u", source, StringComparison.Ordinal);
        Assert.Contains("Memory::Read32(base + 0x10u) != base + 0x00000080u", source, StringComparison.Ordinal);
        Assert.Contains("if (!QuickMatch_quick_match(base)) continue;", source, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::ResolveDeferredReads(base, kFullImageSize)", source, StringComparison.Ordinal);
    }

    [Fact]
    public void RegistrarValidatesRuntimeLinkedPointersAndRelocationPayloads()
    {
        var file = RsoFile.Parse(BuildRso());
        var compilation = RsoDynamicTemplateCompiler.Compile(file,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions("linked-layout", 0x7F000000u, new HashSet<int> { 1 }));
        var source = RsoDynamicTemplateSourceWriter.WriteRegistrarSource(file, compilation);

        Assert.Contains("DynamicModule::ReadImage32(image, 0x0000000Cu) != base + 0x00000058u", source, StringComparison.Ordinal);
        Assert.Contains("DynamicModule::ReadImage32(image, 0x00000060u) != base + 0x000000A0u", source, StringComparison.Ordinal);
        Assert.Contains("const auto validateRelocationTable =", source, StringComparison.Ordinal);
        Assert.Contains("DynamicModule::ReadImage32(kExpected_linked_layout, entryOffset)", source, StringComparison.Ordinal);
        Assert.Contains("if (DynamicModule::ReadImage32(image, entryOffset) != base + patchOffset) return false;", source, StringComparison.Ordinal);
        Assert.Contains("const uint32_t sectionEntry = 0x00000058u + symbolIndex * 0x8u;", source, StringComparison.Ordinal);
        Assert.Contains("const uint32_t importEntry = 0x00000128u + symbolIndex * 0xCu;", source, StringComparison.Ordinal);
        Assert.Contains("if (!validateRelocationField(patchOffset, target, type)) return false;", source, StringComparison.Ordinal);
        Assert.Contains("if (!validateRelocationTable(0x00000100u, 0x00000001u, false)) return false;", source, StringComparison.Ordinal);
        Assert.Contains("if (!validateRelocationTable(0x0000010Cu, 0x00000001u, true)) return false;", source, StringComparison.Ordinal);
    }

    [Fact]
    public void RegistrarStructurallyValidatesPreservedUnresolvedRelocations()
    {
        var file = RsoFile.Parse(BuildRso());
        var compilation = RsoDynamicTemplateCompiler.Compile(
            file,
            new MapProvider(),
            new RsoDynamicTemplateCompileOptions(
                "unresolved-layout",
                0x7F000000u,
                new HashSet<int> { 1 },
                PreserveUnresolvedImports: true));
        var source = RsoDynamicTemplateSourceWriter.WriteRegistrarSource(file, compilation);

        Assert.Contains("const uint32_t patchOffset = canonicalPatch >= 0x7F000000u", source, StringComparison.Ordinal);
        Assert.Contains("? canonicalPatch - 0x7F000000u : canonicalPatch;", source, StringComparison.Ordinal);
        Assert.Contains("if (DynamicModule::ReadImage32(image, entryOffset) != base + patchOffset) return false;", source, StringComparison.Ordinal);
        Assert.Contains("if (external && kResolvedImports_unresolved_layout[symbolIndex] == 0)", source, StringComparison.Ordinal);
        Assert.DoesNotContain("ReadImage32(image, entryOffset) != DynamicModule::ReadImage32(kExpected_unresolved_layout, entryOffset)", source, StringComparison.Ordinal);
    }

    [Fact]
    public void RegistrarUsesFullImageForDiscoveryAndRetainedImageForSteadyStateValidation()
    {
        var file = RsoFile.Parse(BuildRso());
        var compilation = RsoDynamicTemplateCompiler.Compile(
            file,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions(
                "fixed-image",
                0x7F000000u,
                new HashSet<int> { 1 },
                RuntimeImageSize: 0x100u));
        var source = RsoDynamicTemplateSourceWriter.WriteRegistrarSource(file, compilation);

        Assert.Equal(0x100u, compilation.TemplateContext.ImageSize);
        Assert.Contains("constexpr uint32_t kFullImageSize = 0x00000200u;", source, StringComparison.Ordinal);
        Assert.Contains("constexpr uint32_t kRetainedImageSize = 0x00000100u;", source, StringComparison.Ordinal);
        Assert.Contains("MatchMaskedImage(image, kExpected_fixed_image, kCompareMask_fixed_image, kFullImageSize)", source, StringComparison.Ordinal);
        Assert.Contains("MatchMaskedImage(image, kExpected_fixed_image, kCompareMask_fixed_image, kRetainedImageSize)", source, StringComparison.Ordinal);
        Assert.Contains("&Resolve_fixed_image", source, StringComparison.Ordinal);
        Assert.Contains("&ValidateRetained_fixed_image", source, StringComparison.Ordinal);
        Assert.Contains("&ValidateDispatch_fixed_image", source, StringComparison.Ordinal);

        var dispatchStart = source.IndexOf("bool ValidateDispatch_fixed_image", StringComparison.Ordinal);
        var retainedStart = source.IndexOf("bool ValidateRetained_fixed_image", StringComparison.Ordinal);
        Assert.True(dispatchStart >= 0 && retainedStart > dispatchStart);
        var dispatchBody = source[dispatchStart..retainedStart];
        Assert.Contains("QuickMatch_fixed_image(base)", dispatchBody, StringComparison.Ordinal);
        Assert.DoesNotContain("MatchMaskedImage", dispatchBody, StringComparison.Ordinal);
        Assert.DoesNotContain("ResolveDeferredReads", dispatchBody, StringComparison.Ordinal);

        // The full discovery pass still validates both relocation tables even
        // though mode-2 fixed RSO storage ends immediately before them. The
        // retained validator is emitted after that full matcher and therefore
        // cannot depend on discarded relocation/import metadata.
        var resolveStart = source.IndexOf("bool Resolve_fixed_image", StringComparison.Ordinal);
        Assert.True(retainedStart >= 0 && resolveStart > retainedStart);
        var retainedBody = source[retainedStart..resolveStart];
        Assert.DoesNotContain("validateRelocationTable(0x00000100u", retainedBody, StringComparison.Ordinal);
        Assert.DoesNotContain("validateRelocationTable(0x0000010Cu", retainedBody, StringComparison.Ordinal);

        var fullBody = source[..retainedStart];
        Assert.Contains("validateRelocationTable(0x00000100u, 0x00000001u, false)", fullBody, StringComparison.Ordinal);
        Assert.Contains("validateRelocationTable(0x0000010Cu, 0x00000001u, true)", fullBody, StringComparison.Ordinal);
    }

    [Fact]
    public void RetainedRuntimeImageMustContainSectionTableEvenWhenCodeFits()
    {
        var bytes = BuildRso();
        Array.Copy(bytes, 0x58, bytes, 0x180, 4 * 8);
        WriteU32(bytes, 0x0C, 0x180u);
        var file = RsoFile.Parse(bytes);
        var error = Assert.Throws<InvalidDataException>(() => RsoDynamicTemplateCompiler.Compile(
            file, new MapProvider(("Ext", 0x80500000u)),
            new RsoDynamicTemplateCompileOptions("truncated-layout", 0x7F000000u,
                new HashSet<int> { 1 }, RuntimeImageSize: 0x100u)));
        Assert.Contains("section table", error.Message, StringComparison.Ordinal);
    }

    private static bool SignatureMatches(RsoDynamicTemplateSignature signature, byte[] image)
    {
        if (signature.Expected.Length != image.Length) return false;
        for (var index = 0; index < image.Length; ++index)
        {
            if (((image[index] ^ signature.Expected[index]) & signature.CompareMask[index]) != 0)
                return false;
        }
        return true;
    }

    private static byte[] BuildRso()
    {
        var bytes = new byte[0x200];
        WriteU32(bytes, 0x08, 4u);
        WriteU32(bytes, 0x0C, 0x58u);
        WriteU32(bytes, 0x10, 0x80u);
        WriteU32(bytes, 0x14, 14u);
        WriteU32(bytes, 0x18, 1u);
        WriteU32(bytes, 0x1C, 0x10u);
        WriteU32(bytes, 0x30, 0x100u);
        WriteU32(bytes, 0x34, 0x0Cu);
        WriteU32(bytes, 0x38, 0x10Cu);
        WriteU32(bytes, 0x3C, 0x0Cu);
        WriteU32(bytes, 0x40, 0x118u);
        WriteU32(bytes, 0x44, 0x10u);
        WriteU32(bytes, 0x48, 0x140u);
        WriteU32(bytes, 0x4C, 0x128u);
        WriteU32(bytes, 0x50, 0x0Cu);
        WriteU32(bytes, 0x54, 0x150u);

        WriteU32(bytes, 0x58 + 0x08, 0xA0u);
        WriteU32(bytes, 0x58 + 0x0C, 0x10u);
        WriteU32(bytes, 0x58 + 0x10, 0xC0u);
        WriteU32(bytes, 0x58 + 0x14, 0x20u);
        WriteU32(bytes, 0x58 + 0x18, 0u);
        WriteU32(bytes, 0x58 + 0x1C, 0x10u);
        Encoding.ASCII.GetBytes("synthetic.rso\0").CopyTo(bytes, 0x80);

        WriteU32(bytes, 0xA0, 0x48000001u); // bl helper, relocated internally
        WriteU32(bytes, 0xA4, 0x48000001u); // bl Ext, relocated externally
        WriteU32(bytes, 0xA8, 0x4E800020u); // blr
        WriteU32(bytes, 0xAC, 0x4E800020u); // helper: blr

        WriteU32(bytes, 0x100, 0xA0u);
        WriteU32(bytes, 0x104, (1u << 8) | (uint)RsoRelocationType.R_PPC_REL24);
        WriteU32(bytes, 0x108, 0x0Cu);
        WriteU32(bytes, 0x10C, 0xA4u);
        WriteU32(bytes, 0x110, (uint)RsoRelocationType.R_PPC_REL24);
        WriteU32(bytes, 0x114, 0u);

        WriteU32(bytes, 0x118, 0u);
        WriteU32(bytes, 0x11C, 0u);
        WriteU32(bytes, 0x120, 1u);
        WriteU32(bytes, 0x124, 0x737FEu);
        WriteU32(bytes, 0x128, 0u);
        WriteU32(bytes, 0x12C, 0u);
        WriteU32(bytes, 0x130, 0u);
        Encoding.ASCII.GetBytes("main\0").CopyTo(bytes, 0x140);
        Encoding.ASCII.GetBytes("Ext\0").CopyTo(bytes, 0x150);
        return bytes;
    }

    private static void WriteU32(byte[] data, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(offset, 4), value);

    private sealed class MapProvider(params (string Name, uint Address)[] entries) : IRsoSymbolProvider
    {
        private readonly Dictionary<string, uint> _map = entries.ToDictionary(static entry => entry.Name, static entry => entry.Address);
        public bool TryResolve(string name, out uint address) => _map.TryGetValue(name, out address);
    }
}
