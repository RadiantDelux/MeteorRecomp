using System.Buffers.Binary;
using System.Collections.Generic;
using Translator.Core.Ir;
using Translator.Core.Loading;
using Translator.Core.Translation;
using Xunit;

namespace Translator.Tests;

public class DynamicModuleReviewTests
{
    private static readonly DynamicModuleTemplateContext Template = new(
        "review-template",
        CanonicalImageBase: 0x71000000u,
        ImageSize: 0x1000u,
        CanonicalBssBase: 0x72000000u,
        BssSize: 0x100u);

    [Fact]
    public void PpcBlAndMflrKeepTheRuntimeInstanceReturnAddress()
    {
        const uint canonicalBase = 0x71000000u;
        const uint entry = canonicalBase + 0x100u;
        const uint callee = canonicalBase + 0x200u;
        var memory = new byte[0x1000];

        WriteWord(memory, canonicalBase, entry, EncodeB(entry, callee, link: true));
        WriteWord(memory, canonicalBase, entry + 4u, 0x7C8802A6u); // mflr r4
        WriteWord(memory, canonicalBase, entry + 8u, 0x4E800020u); // blr
        WriteWord(memory, canonicalBase, callee, 0x4E800020u);     // callee: blr

        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            new AddressRange(0, 0),
            "dynamic-review",
            memoryBase: canonicalBase);

        var result = new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with
            {
                MaxBytes = 0x10,
                AllowUnsupportedInstructions = true,
                EnableLeafInlining = false,
                KnownFunctionEntryPoints = new HashSet<uint> { entry, callee },
                DynamicModuleTemplate = Template,
            });

        Assert.Contains(
            "ctx->lr = DynamicModule::RuntimeAddressFromOffset(ctx, 0x00000104u);",
            result.CxxCode,
            StringComparison.Ordinal);
        Assert.Contains(
            "InvokeCurrentDynamicModuleFunction(0x00000200u, ctx);",
            result.CxxCode,
            StringComparison.Ordinal);

        var mflrCopy = Assert.Single(
            result.LinearIr.Blocks
                .SelectMany(static block => block.Instructions)
                .OfType<IrAssign>()
                .Where(static assign => assign.Destination.Equals("r4", StringComparison.OrdinalIgnoreCase)));
        Assert.Equal("lr", mflrCopy.Value.RegisterName);
    }

    [Fact]
    public void OnlyRelocationTaggedPointerMaterializationIsRebased()
    {
        const uint canonicalBase = 0x71000000u;
        const uint entry = canonicalBase + 0x100u;
        const uint canonicalTarget = 0x72000040u;
        var memory = new byte[0x1000];

        // Tagged pair: lis r3,0x7200 / addi r3,r3,0x40.
        WriteWord(memory, canonicalBase, entry + 0x00u, 0x3C607200u);
        WriteWord(memory, canonicalBase, entry + 0x04u, 0x38630040u);
        // Numerically identical pointer construction, but deliberately without
        // relocation provenance. It must remain ordinary integer arithmetic.
        WriteWord(memory, canonicalBase, entry + 0x08u, 0x3C807200u);
        WriteWord(memory, canonicalBase, entry + 0x0Cu, 0x38840040u);
        WriteWord(memory, canonicalBase, entry + 0x10u, 0x4E800020u);

        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            new AddressRange(0, 0),
            "dynamic-pointer-provenance",
            memoryBase: canonicalBase);
        var template = Template with
        {
            AddressRelocations =
            [
                new DynamicModuleAddressRelocation(
                    entry,
                    canonicalTarget,
                    DynamicModuleAddressRelocationKind.High16),
                new DynamicModuleAddressRelocation(
                    entry + 4u,
                    canonicalTarget,
                    DynamicModuleAddressRelocationKind.Low16),
            ]
        };

        var result = new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with
            {
                MaxBytes = 0x20,
                AllowUnsupportedInstructions = true,
                EnableLeafInlining = false,
                DynamicModuleTemplate = template,
            });

        var assignments = result.LinearIr.Blocks
            .SelectMany(static block => block.Instructions)
            .OfType<IrAssign>()
            .ToArray();
        var tagged = Assert.Single(assignments.Where(static assign =>
            assign.Destination.Equals("r3", StringComparison.OrdinalIgnoreCase) &&
            assign.Value.Kind == "dynamic_address"));
        Assert.Equal(canonicalTarget, unchecked((uint)tagged.Value.Constant!.Value));
        Assert.DoesNotContain(assignments, static assign =>
            assign.Destination.Equals("r4", StringComparison.OrdinalIgnoreCase) &&
            assign.Value.Kind == "dynamic_address");
        Assert.Contains(
            "DynamicModule::RebaseCanonicalAddress(ctx, 0x72000040u)",
            result.CxxCode,
            StringComparison.Ordinal);
    }

    [Fact]
    public void UnpairedLowPointerRelocationFailsClosed()
    {
        const uint canonicalBase = 0x71000000u;
        const uint entry = canonicalBase + 0x100u;
        var memory = new byte[0x1000];
        WriteWord(memory, canonicalBase, entry, 0x38630040u); // addi r3,r3,0x40
        WriteWord(memory, canonicalBase, entry + 4u, 0x4E800020u);
        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            new AddressRange(0, 0),
            "dynamic-unpaired-pointer",
            memoryBase: canonicalBase);
        var template = Template with
        {
            AddressRelocations =
            [
                new DynamicModuleAddressRelocation(
                    entry,
                    0x72000040u,
                    DynamicModuleAddressRelocationKind.Low16),
            ]
        };

        Assert.Throws<InvalidDataException>(() => new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with
            {
                MaxBytes = 0x10,
                AllowUnsupportedInstructions = true,
                EnableLeafInlining = false,
                DynamicModuleTemplate = template,
            }));
    }

    [Fact]
    public void RelocatedLisPlusMemoryLowUsesCanonicalMemoryUntilRuntimeRebase()
    {
        const uint canonicalBase = 0x71000000u;
        const uint entry = canonicalBase + 0x100u;
        const uint canonicalTarget = 0x72000040u;
        var memory = new byte[0x1000];
        WriteWord(memory, canonicalBase, entry, 0x3CC07200u);       // lis r6,0x7200
        WriteWord(memory, canonicalBase, entry + 4u, 0x80660040u); // lwz r3,0x40(r6)
        WriteWord(memory, canonicalBase, entry + 8u, 0x4E800020u); // blr
        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            new AddressRange(0, 0),
            "dynamic-memory-provenance",
            memoryBase: canonicalBase);
        var template = Template with
        {
            AddressRelocations =
            [
                new DynamicModuleAddressRelocation(entry, canonicalTarget, DynamicModuleAddressRelocationKind.High16),
                new DynamicModuleAddressRelocation(entry + 4u, canonicalTarget, DynamicModuleAddressRelocationKind.Low16),
            ]
        };

        var result = new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with
            {
                MaxBytes = 0x10,
                AllowUnsupportedInstructions = true,
                EnableLeafInlining = false,
                DynamicModuleTemplate = template,
            });

        Assert.Contains("DynamicModule::RebaseCanonicalAddress", result.CxxCode, StringComparison.Ordinal);
        Assert.DoesNotContain("dynamic_address", result.CxxCode, StringComparison.Ordinal);
    }

    [Fact]
    public void PurePpcHelperBetweenRelocationHalvesPreservesGprProvenance()
    {
        const uint canonicalBase = 0x71000000u;
        const uint entry = canonicalBase + 0x100u;
        const uint canonicalTarget = 0x72000040u;
        var memory = new byte[0x1000];

        WriteWord(memory, canonicalBase, entry + 0x00u, 0x3C807200u); // lis r4,0x7200
        WriteWord(memory, canonicalBase, entry + 0x04u, EncodePrimary59(3, 3, 0, 0, 20)); // fsubs f3,f3,f0
        WriteWord(memory, canonicalBase, entry + 0x08u, 0x80640040u); // lwz r3,0x40(r4)
        WriteWord(memory, canonicalBase, entry + 0x0Cu, 0x4E800020u); // blr

        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            new AddressRange(0, 0),
            "dynamic-helper-relocation-provenance",
            memoryBase: canonicalBase);
        var template = Template with
        {
            AddressRelocations =
            [
                new DynamicModuleAddressRelocation(entry, canonicalTarget, DynamicModuleAddressRelocationKind.High16),
                new DynamicModuleAddressRelocation(entry + 8u, canonicalTarget, DynamicModuleAddressRelocationKind.Low16),
            ]
        };

        var result = new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with
            {
                MaxBytes = 0x20,
                AllowUnsupportedInstructions = true,
                EnableLeafInlining = false,
                DynamicModuleTemplate = template,
            });

        Assert.Contains("DynamicModule::RebaseCanonicalAddress", result.CxxCode, StringComparison.Ordinal);
    }

    [Fact]
    public void StandaloneHighAddressRelocationFailsClosed()
    {
        const uint canonicalBase = 0x71000000u;
        const uint entry = canonicalBase + 0x100u;
        var memory = new byte[0x1000];
        WriteWord(memory, canonicalBase, entry, 0x3C607200u);       // lis r3,0x7200
        WriteWord(memory, canonicalBase, entry + 4u, 0x4E800020u); // blr
        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            AddressRange.FromStartAndSize(canonicalBase, (uint)memory.Length),
            new AddressRange(0, 0),
            "dynamic-high-only",
            memoryBase: canonicalBase);
        var template = Template with
        {
            AddressRelocations =
            [new DynamicModuleAddressRelocation(entry, 0x72000040u, DynamicModuleAddressRelocationKind.High16)]
        };

        Assert.Throws<InvalidDataException>(() => new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with
            {
                MaxBytes = 0x10,
                AllowUnsupportedInstructions = true,
                EnableLeafInlining = false,
                DynamicModuleTemplate = template,
            }));
    }

    private static uint EncodeB(uint address, uint target, bool link) =>
        (18u << 26) | ((target - address) & 0x03FFFFFCu) | (link ? 1u : 0u);

    private static uint EncodePrimary59(uint frt, uint fra, uint frb, uint frc, uint xo, bool rc = false) =>
        (59u << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1) | (rc ? 1u : 0u);

    private static void WriteWord(byte[] memory, uint memoryBase, uint address, uint value)
    {
        var offset = checked((int)(address - memoryBase));
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(offset, 4), value);
    }
}
