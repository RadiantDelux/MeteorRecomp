using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using Translator.Core.Parsing.Rso;

namespace Translator.Tests;

public class RsoImageBuilderTests
{
    private const uint ImageBase = 0x80010000u;
    private const uint BssBase = 0x81234000u;

    [Fact]
    public void BuildsDeterministicLayoutBssLifecycleAndExports()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_NONE);
        var file = RsoFile.Parse(raw);
        var linked = RsoImageBuilder.Build(file, ImageBase, BssBase, new MapProvider(("Ext", 0x80500000u)));

        Assert.Equal(ImageBase + 0x58u, ReadU32(linked.Image, 0x0C));
        Assert.Equal(ImageBase + 0x80u, ReadU32(linked.Image, 0x10));
        Assert.Equal(ImageBase + 0x100u, ReadU32(linked.Image, 0x38));
        Assert.Equal(ImageBase + 0x120u, ReadU32(linked.Image, 0x4C));

        Assert.Equal(ImageBase + 0xA0u, linked.Sections[1].Address);
        Assert.Equal(ImageBase + 0xC0u, linked.Sections[2].Address);
        Assert.Equal(BssBase, linked.Sections[3].Address);
        Assert.True(linked.Sections[3].IsBss);
        Assert.Equal(3, linked.Image[0x23]);

        Assert.Equal(ImageBase + 0xA0u, linked.PrologAddress);
        Assert.Equal(ImageBase + 0xA4u, linked.EpilogAddress);
        Assert.Equal(ImageBase + 0xA8u, linked.UnresolvedAddress);
        Assert.Equal(ImageBase + 0xA4u, Assert.Single(linked.Exports).Address);

        Assert.Equal(new byte[0x10], linked.Bss);
        Assert.Equal(SHA256.HashData(raw), linked.PreLinkSha256);
        Assert.Equal(raw, file.RawData.ToArray());
    }

    [Fact]
    public void PreservesBssOnlyWhenRequested()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_NONE);
        var file = RsoFile.Parse(raw);
        var initial = Enumerable.Range(1, 0x10).Select(value => (byte)value).ToArray();

        var cleared = RsoImageBuilder.Build(
            file,
            ImageBase,
            BssBase,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoImageBuildOptions { InitialBss = initial });
        Assert.Equal(new byte[0x10], cleared.Bss);

        var preserved = RsoImageBuilder.Build(
            file,
            ImageBase,
            BssBase,
            new MapProvider(("Ext", 0x80500000u)),
            new RsoImageBuildOptions { PreserveBss = true, InitialBss = initial });
        Assert.Equal(initial, preserved.Bss);
    }

    [Theory]
    [InlineData(RsoRelocationType.R_PPC_NONE)]
    [InlineData(RsoRelocationType.R_PPC_ADDR32)]
    [InlineData(RsoRelocationType.R_PPC_ADDR24)]
    [InlineData(RsoRelocationType.R_PPC_ADDR16)]
    [InlineData(RsoRelocationType.R_PPC_ADDR16_LO)]
    [InlineData(RsoRelocationType.R_PPC_ADDR16_HI)]
    [InlineData(RsoRelocationType.R_PPC_ADDR16_HA)]
    [InlineData(RsoRelocationType.R_PPC_ADDR14)]
    [InlineData(RsoRelocationType.R_PPC_ADDR14_BRTAKEN)]
    [InlineData(RsoRelocationType.R_PPC_ADDR14_BRNTAKEN)]
    [InlineData(RsoRelocationType.R_PPC_REL24)]
    [InlineData(RsoRelocationType.R_PPC_REL14)]
    [InlineData(RsoRelocationType.R_PPC_REL14_BRTAKEN)]
    [InlineData(RsoRelocationType.R_PPC_REL14_BRNTAKEN)]
    public void AppliesRetailRelocationFormula(RsoRelocationType type)
    {
        var raw = BuildRso(type);
        var file = RsoFile.Parse(raw);
        var patchAddress = ImageBase + 0xA0u;
        var target = ImageBase + 0x1000u;
        var linked = RsoImageBuilder.Build(file, ImageBase, BssBase, new MapProvider(("Ext", target)));

        var expected = ExpectedPatch(type, 0x48000001u, patchAddress, target);
        if (type is RsoRelocationType.R_PPC_ADDR16 or RsoRelocationType.R_PPC_ADDR16_LO or
            RsoRelocationType.R_PPC_ADDR16_HI or RsoRelocationType.R_PPC_ADDR16_HA)
        {
            Assert.Equal((ushort)expected, ReadU16(linked.Image, 0xA0));
        }
        else
        {
            Assert.Equal(expected, ReadU32(linked.Image, 0xA0));
        }

        Assert.Equal(patchAddress, ReadU32(linked.Image, 0x100));
        var provenance = Assert.Single(linked.Relocations);
        Assert.Equal(patchAddress, provenance.PatchAddress);
        Assert.Equal(target, provenance.ResolvedTarget);
        Assert.Equal("Ext", provenance.SymbolName);
    }

    [Fact]
    public void Rel24UsesRetailOverflowSentinel()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_REL24);
        var linked = RsoImageBuilder.Build(
            RsoFile.Parse(raw),
            0x80000000u,
            BssBase,
            new MapProvider(("Ext", 0x90000000u)));

        Assert.Equal(0x4BFFFFFDu, ReadU32(linked.Image, 0xA0));
    }

    [Fact]
    public void UsesUnresolvedHandlerWithoutClaimingImportWasResolved()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_REL24);
        var linked = RsoImageBuilder.Build(
            RsoFile.Parse(raw),
            ImageBase,
            BssBase,
            options: new RsoImageBuildOptions { UnresolvedSymbolAddress = 0x8050F000u });

        var import = Assert.Single(linked.Imports);
        Assert.False(import.IsResolved);
        Assert.Equal(0x8050F000u, import.Address);
        Assert.Equal(0x8050F000u, ReadU32(linked.Image, 0x124));
    }

    [Fact]
    public void RejectsUnresolvedImportWithoutFallback()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_NONE);
        Assert.Throws<InvalidDataException>(() => RsoImageBuilder.Build(RsoFile.Parse(raw), ImageBase, BssBase));
    }

    [Fact]
    public void PreservesUnresolvedImportAndRelocationGroupWhenRequested()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_REL24);
        var originalInstruction = ReadU32(raw, 0xA0);
        var originalImportAddress = ReadU32(raw, 0x124);
        var originalRelocationOffset = ReadU32(raw, 0x100);

        var linked = RsoImageBuilder.Build(
            RsoFile.Parse(raw),
            ImageBase,
            BssBase,
            options: new RsoImageBuildOptions { PreserveUnresolvedImports = true });

        var import = Assert.Single(linked.Imports);
        Assert.False(import.IsResolved);
        Assert.Equal(originalImportAddress, import.Address);
        Assert.Equal(originalImportAddress, ReadU32(linked.Image, 0x124));
        Assert.Equal(originalInstruction, ReadU32(linked.Image, 0xA0));
        Assert.Equal(originalRelocationOffset, ReadU32(linked.Image, 0x100));
        Assert.Empty(linked.Relocations);
    }

    [Fact]
    public void RejectsSdaRelocationRatherThanInventingContext()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_EMB_SDA21);
        Assert.Throws<NotSupportedException>(() => RsoImageBuilder.Build(
            RsoFile.Parse(raw),
            ImageBase,
            BssBase,
            new MapProvider(("Ext", 0x80500000u))));
    }

    [Fact]
    public void SameTemplateLinksAtTwoIndependentBases()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_REL24);
        var file = RsoFile.Parse(raw);
        var provider = new MapProvider(("Ext", 0x80500000u));

        var first = RsoImageBuilder.Build(file, 0x80010000u, 0x81234000u, provider);
        var second = RsoImageBuilder.Build(file, 0x81010000u, 0x81334000u, provider);

        Assert.Equal(first.PreLinkSha256, second.PreLinkSha256);
        Assert.Equal(0x01000000u, second.Sections[1].Address - first.Sections[1].Address);
        Assert.NotEqual(ReadU32(first.Image, 0x0C), ReadU32(second.Image, 0x0C));
        Assert.Equal(raw, file.RawData.ToArray());
    }

    [Fact]
    public void RejectsMalformedExternalRelocationGroup()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_NONE);
        WriteU32(raw, 0x128, 1u);
        var file = RsoFile.Parse(raw);

        Assert.Throws<InvalidDataException>(() => RsoImageBuilder.Build(
            file,
            ImageBase,
            BssBase,
            new MapProvider(("Ext", 0x80500000u))));
    }

    [Fact]
    public void RejectsMultipleNonEmptyBssSections()
    {
        var raw = BuildRso(RsoRelocationType.R_PPC_NONE);
        WriteU32(raw, 0x08, 5u);
        WriteU32(raw, 0x58 + 4 * 8, 0u);
        WriteU32(raw, 0x58 + 4 * 8 + 4, 0x10u);
        var file = RsoFile.Parse(raw);

        Assert.Throws<InvalidDataException>(() => RsoImageBuilder.Build(
            file,
            ImageBase,
            BssBase,
            new MapProvider(("Ext", 0x80500000u))));
    }

    private static uint ExpectedPatch(
        RsoRelocationType type,
        uint original,
        uint patchAddress,
        uint target)
    {
        return type switch
        {
            RsoRelocationType.R_PPC_NONE => original,
            RsoRelocationType.R_PPC_ADDR32 => target,
            RsoRelocationType.R_PPC_ADDR24 => (original & 0xFC000003u) | (target & 0x03FFFFFCu),
            RsoRelocationType.R_PPC_ADDR16 or RsoRelocationType.R_PPC_ADDR16_LO => target & 0xFFFFu,
            RsoRelocationType.R_PPC_ADDR16_HI => target >> 16,
            RsoRelocationType.R_PPC_ADDR16_HA => (target + 0x8000u) >> 16,
            RsoRelocationType.R_PPC_ADDR14 or RsoRelocationType.R_PPC_ADDR14_BRTAKEN or
                RsoRelocationType.R_PPC_ADDR14_BRNTAKEN => (original & 0xFFFF0003u) | (target & 0x0000FFFCu),
            RsoRelocationType.R_PPC_REL24 =>
                (original & 0xFC000003u) | ((target - patchAddress) & 0x03FFFFFCu),
            RsoRelocationType.R_PPC_REL14 or RsoRelocationType.R_PPC_REL14_BRTAKEN or
                RsoRelocationType.R_PPC_REL14_BRNTAKEN =>
                (original & 0xFFFF0003u) | ((target - patchAddress) & 0x0000FFFCu),
            _ => throw new ArgumentOutOfRangeException(nameof(type)),
        };
    }

    private static byte[] BuildRso(RsoRelocationType relocationType)
    {
        var bytes = new byte[0x200];
        WriteU32(bytes, 0x08, 4u);
        WriteU32(bytes, 0x0C, 0x58u);
        WriteU32(bytes, 0x10, 0x80u);
        WriteU32(bytes, 0x14, 9u);
        WriteU32(bytes, 0x18, 1u);
        WriteU32(bytes, 0x1C, 0x10u);
        bytes[0x20] = 1;
        bytes[0x21] = 1;
        bytes[0x22] = 1;
        bytes[0x23] = 0;
        WriteU32(bytes, 0x24, 0u);
        WriteU32(bytes, 0x28, 4u);
        WriteU32(bytes, 0x2C, 8u);
        WriteU32(bytes, 0x30, 0u);
        WriteU32(bytes, 0x34, 0u);
        WriteU32(bytes, 0x38, 0x100u);
        WriteU32(bytes, 0x3C, 0x0Cu);
        WriteU32(bytes, 0x40, 0x110u);
        WriteU32(bytes, 0x44, 0x10u);
        WriteU32(bytes, 0x48, 0x140u);
        WriteU32(bytes, 0x4C, 0x120u);
        WriteU32(bytes, 0x50, 0x0Cu);
        WriteU32(bytes, 0x54, 0x150u);

        WriteU32(bytes, 0x58 + 0x08, 0xA0u);
        WriteU32(bytes, 0x58 + 0x0C, 0x20u);
        WriteU32(bytes, 0x58 + 0x10, 0xC0u);
        WriteU32(bytes, 0x58 + 0x14, 0x20u);
        WriteU32(bytes, 0x58 + 0x18, 0u);
        WriteU32(bytes, 0x58 + 0x1C, 0x10u);

        Encoding.Latin1.GetBytes("test.rso\0").CopyTo(bytes, 0x80);
        WriteU32(bytes, 0xA0, 0x48000001u);

        WriteU32(bytes, 0x100, 0xA0u);
        WriteU32(bytes, 0x104, (uint)(byte)relocationType);
        WriteU32(bytes, 0x108, 0u);

        WriteU32(bytes, 0x110, 0u);
        WriteU32(bytes, 0x114, 4u);
        WriteU32(bytes, 0x118, 1u);
        WriteU32(bytes, 0x11C, 0x737FEu);
        Encoding.Latin1.GetBytes("main\0").CopyTo(bytes, 0x140);

        WriteU32(bytes, 0x120, 0u);
        WriteU32(bytes, 0x124, 0u);
        WriteU32(bytes, 0x128, 0u);
        Encoding.Latin1.GetBytes("Ext\0").CopyTo(bytes, 0x150);
        return bytes;
    }

    private static ushort ReadU16(byte[] bytes, int offset) =>
        BinaryPrimitives.ReadUInt16BigEndian(bytes.AsSpan(offset, 2));

    private static uint ReadU32(byte[] bytes, int offset) =>
        BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(offset, 4));

    private static void WriteU32(byte[] bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(offset, 4), value);

    private sealed class MapProvider(params (string Name, uint Address)[] entries) : IRsoSymbolProvider
    {
        private readonly Dictionary<string, uint> _addresses = entries.ToDictionary(entry => entry.Name, entry => entry.Address);

        public bool TryResolve(string name, out uint address) => _addresses.TryGetValue(name, out address);
    }
}
