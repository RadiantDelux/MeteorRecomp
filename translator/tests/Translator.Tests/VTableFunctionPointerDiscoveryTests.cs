using System.Buffers.Binary;
using Translator.Core.Analysis;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Loading;
using Xunit;

namespace Translator.Tests;

public sealed class VTableFunctionPointerDiscoveryTests
{
    [Fact]
    public void DiscoversCodeWarriorVTableRunsButNotBareJumpTables()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;

        var dataBytes = new byte[0x40];
        WriteU32(dataBytes, 0x00, data + 0x30); // RTTI/data pointer
        WriteU32(dataBytes, 0x04, 0);           // this adjustment
        WriteU32(dataBytes, 0x08, text + 0x00); // vtable method 1
        WriteU32(dataBytes, 0x0C, text + 0x08); // vtable method 2
        WriteU32(dataBytes, 0x10, 0x12345678u);

        // A raw jump table has consecutive code pointers but no RTTI/adjustment preamble.
        WriteU32(dataBytes, 0x18, text + 0x10);
        WriteU32(dataBytes, 0x1C, text + 0x18);
        WriteU32(dataBytes, 0x30, data + 0x30);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x9421FFE0u, 0x4E800020u, // 0x00
                    0x38600001u, 0x4E800020u, // 0x08
                    0x38600002u, 0x4E800020u, // 0x10
                    0x38600003u, 0x4E800020u),// 0x18
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);

        var result = VTableFunctionPointerDiscovery.Discover(dol, image);

        Assert.Equal(1, result.TableSegmentCount);
        Assert.Equal(2, result.PointerEntryCount);
        Assert.Equal([text, text + 0x08], result.Targets);
        Assert.DoesNotContain(text + 0x10, result.Targets);
        Assert.DoesNotContain(text + 0x18, result.Targets);
    }

    [Fact]
    public void StaticPointerDiscoverySeparatesDenseRunsFromIsolatedCallbacks()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;

        var dataBytes = new byte[0x30];
        WriteU32(dataBytes, 0x00, text + 0x00);
        WriteU32(dataBytes, 0x04, text + 0x08);
        WriteU32(dataBytes, 0x08, 2);
        WriteU32(dataBytes, 0x0C, text + 0x10);
        WriteU32(dataBytes, 0x10, 0);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x9421FFE0u, 0x4E800020u,
                    0x38600001u, 0x4E800020u,
                    0x38600002u, 0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);

        var result = StaticFunctionPointerTableDiscovery.Discover(dol, image);

        var table = Assert.Single(result.Tables);
        Assert.Equal([text, text + 0x08], table.Targets);
        Assert.Equal([text + 0x10], result.IsolatedTargets);
    }

    [Fact]
    public void StaticPointerDiscoveryRecognizesDedicatedCtorSectionWithoutSentinel()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;

        var dataBytes = new byte[0x18];
        WriteU32(dataBytes, 0x00, text + 0x00);
        WriteU32(dataBytes, 0x04, text + 0x08);
        WriteU32(dataBytes, 0x08, text + 0x10);
        // DOL .ctors/.dtors sections commonly end in zero padding rather than
        // carrying the CodeWarrior -1 sentinel inside the section itself.
        WriteU32(dataBytes, 0x0C, 0);
        WriteU32(dataBytes, 0x10, 0);
        WriteU32(dataBytes, 0x14, 0);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x38600000u, 0x4E800020u,
                    0x38600001u, 0x4E800020u,
                    0x38600002u, 0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);

        var result = StaticFunctionPointerTableDiscovery.Discover(dol, image);
        var table = Assert.Single(result.Tables);

        Assert.Equal(data, table.Address);
        Assert.Equal([text, text + 0x08, text + 0x10], table.Targets);
        Assert.True(table.HasDedicatedSectionCallbackShape);
        Assert.False(table.HasCodeWarriorSentinelPreamble);
    }

    [Fact]
    public void StoredNullHeaderVTablePromotesOnlyReturnDelimitedUncoveredSiblings()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;

        var dataBytes = new byte[0x60];
        // Null-header vtable. Two translated anchors bracket three unknown-looking
        // siblings; only text+0x08 has both a raw blr boundary and no translated
        // flow ownership.
        WriteU32(dataBytes, 0x08, text + 0x00); // proven anchor A
        WriteU32(dataBytes, 0x0C, text + 0x08); // safe sibling
        WriteU32(dataBytes, 0x10, text + 0x10); // covered/interior sibling
        WriteU32(dataBytes, 0x14, text + 0x14); // no preceding blr
        WriteU32(dataBytes, 0x18, text + 0x1C); // proven anchor B
        WriteU32(dataBytes, 0x1C, 0x12345678u);

        // Same executable-looking shape as a jump table, but without the null
        // header. Even with two anchors and a blr-delimited candidate it must not
        // receive the relaxed vtable treatment.
        WriteU32(dataBytes, 0x28, 1);
        WriteU32(dataBytes, 0x2C, 2);
        WriteU32(dataBytes, 0x30, text + 0x00);
        WriteU32(dataBytes, 0x34, text + 0x24);
        WriteU32(dataBytes, 0x38, text + 0x1C);
        WriteU32(dataBytes, 0x3C, 0x12345678u);

        // A null header by itself is still insufficient. This table has no stored
        // header-pointer evidence, so its sibling remains data even if one method
        // happens to be independently known.
        WriteU32(dataBytes, 0x48, text + 0x00);
        WriteU32(dataBytes, 0x4C, text + 0x24);
        WriteU32(dataBytes, 0x50, 0x12345678u);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x38600000u, 0x4E800020u, // anchor A
                    0x80630000u, 0x4E800020u, // safe sibling
                    0x38630001u,               // covered candidate
                    0x60000000u, 0x4E800020u, // no-boundary candidate body
                    0x38600002u, 0x4E800020u, // anchor B
                    0x80630004u, 0x4E800020u),// bare-table candidate
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);
        var pointerTables = StaticFunctionPointerTableDiscovery.Discover(dol, image);

        Assert.Contains(pointerTables.Tables, table =>
            table.Address == data + 0x08 && table.HasNullHeaderPreamble);
        Assert.Contains(pointerTables.Tables, table =>
            table.Address == data + 0x30 && !table.HasNullHeaderPreamble);
        Assert.Contains(pointerTables.Tables, table =>
            table.Address == data + 0x48 && table.HasNullHeaderPreamble);

        var result = StoredNullHeaderVTableDiscovery.Discover(
            pointerTables,
            dol,
            image,
            new HashSet<uint> { text + 0x00, text + 0x1C },
            new HashSet<uint> { text + 0x10 },
            new HashSet<uint> { data });

        Assert.Equal([text + 0x08], result.Targets);
        Assert.Equal(1, result.TableSegmentCount);
        Assert.DoesNotContain(text + 0x10, result.Targets); // translated interior
        Assert.DoesNotContain(text + 0x14, result.Targets); // no raw return boundary
        Assert.DoesNotContain(text + 0x24, result.Targets); // bare/unanchored table
    }

    [Fact]
    public void StoredHeaderPointerCanProveNullHeaderVTableWithoutMethodAnchors()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;

        var dataBytes = new byte[0x30];
        // {0,0} header followed by five independent two-instruction methods.
        WriteU32(dataBytes, 0x08, text + 0x04);
        WriteU32(dataBytes, 0x0C, text + 0x0C);
        WriteU32(dataBytes, 0x10, text + 0x14);
        WriteU32(dataBytes, 0x14, text + 0x1C);
        WriteU32(dataBytes, 0x18, text + 0x24);
        WriteU32(dataBytes, 0x1C, 0x12345678u);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x4E800020u,
                    0x38630001u, 0x4E800020u,
                    0x38630002u, 0x4E800020u,
                    0x38630003u, 0x4E800020u,
                    0x38630004u, 0x4E800020u,
                    0x38630005u, 0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);
        var pointerTables = StaticFunctionPointerTableDiscovery.Discover(dol, image);
        var noAnchors = new HashSet<uint>();
        var noCoverage = new HashSet<uint>();

        var withoutStoreEvidence = StoredNullHeaderVTableDiscovery.Discover(
            pointerTables, dol, image, noAnchors, noCoverage);
        Assert.Empty(withoutStoreEvidence.Targets);

        var withStoreEvidence = StoredNullHeaderVTableDiscovery.Discover(
            pointerTables,
            dol,
            image,
            noAnchors,
            noCoverage,
            new HashSet<uint> { data });
        Assert.Equal(
            new[] { text + 0x04, text + 0x0C, text + 0x14, text + 0x1C, text + 0x24 },
            withStoreEvidence.Targets);
        Assert.Equal(1, withStoreEvidence.TableSegmentCount);
    }

    [Fact]
    public void StoredNullHeaderVTablePromotesPackedTailBranchThunks()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;

        var dataBytes = new byte[0x30];
        // Null-header vtable with five two-instruction CodeWarrior-style tail
        // thunks. Four of five methods are independently proven; the remaining
        // method starts immediately after an unconditional non-link branch.
        // Requiring dense pre-existing anchors prevents an unproven branch-stub
        // table from bootstrapping itself merely because its entries have no
        // fallthrough.
        WriteU32(dataBytes, 0x08, text + 0x00);
        WriteU32(dataBytes, 0x0C, text + 0x08);
        WriteU32(dataBytes, 0x10, text + 0x10);
        WriteU32(dataBytes, 0x14, text + 0x18);
        WriteU32(dataBytes, 0x18, text + 0x20);
        WriteU32(dataBytes, 0x1C, 0x12345678u);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x38630001u, 0x48000028u, // thunk 1 -> text+0x2C
                    0x38630002u, 0x48000020u, // thunk 2 -> text+0x2C
                    0x38630003u, 0x48000018u, // thunk 3 -> text+0x2C
                    0x38630004u, 0x48000010u, // thunk 4 -> text+0x2C
                    0x38630005u, 0x48000008u, // thunk 5 -> text+0x2C
                    0x60000000u,
                    0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);
        var pointerTables = StaticFunctionPointerTableDiscovery.Discover(dol, image);

        var result = StoredNullHeaderVTableDiscovery.Discover(
            pointerTables,
            dol,
            image,
            new HashSet<uint> { text, text + 0x08, text + 0x10, text + 0x20 },
            new HashSet<uint>(),
            new HashSet<uint> { data });

        Assert.Equal(
            new[] { text + 0x18 },
            result.Targets);
        Assert.Equal(1, result.TableSegmentCount);
    }

    [Fact]
    public void StoredNullHeaderTailBranchThunksRequireDenseProvenAnchors()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;
        var dataBytes = new byte[0x30];
        WriteU32(dataBytes, 0x08, text + 0x00);
        WriteU32(dataBytes, 0x0C, text + 0x08);
        WriteU32(dataBytes, 0x10, text + 0x10);
        WriteU32(dataBytes, 0x14, text + 0x18);
        WriteU32(dataBytes, 0x18, text + 0x20);
        WriteU32(dataBytes, 0x1C, 0x12345678u);
        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x38630001u, 0x48000028u,
                    0x38630002u, 0x48000020u,
                    0x38630003u, 0x48000018u,
                    0x38630004u, 0x48000010u,
                    0x38630005u, 0x48000008u,
                    0x60000000u,
                    0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);
        var pointerTables = StaticFunctionPointerTableDiscovery.Discover(dol, image);

        var result = StoredNullHeaderVTableDiscovery.Discover(
            pointerTables,
            dol,
            image,
            new HashSet<uint> { text },
            new HashSet<uint>(),
            new HashSet<uint> { data });

        Assert.Empty(result.Targets);
        Assert.Equal(0, result.TableSegmentCount);
    }

    [Fact]
    public void StoredNullHeaderTailBranchThunksCanUseReturnDelimitedAnchorWhenAllTargetsAreTerminal()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;
        var dataBytes = new byte[0x20];
        WriteU32(dataBytes, 0x08, text + 0x04); // proven, follows blr
        WriteU32(dataBytes, 0x0C, text + 0x0C); // sibling, follows tail branch
        WriteU32(dataBytes, 0x10, text + 0x14); // sibling, follows tail branch
        WriteU32(dataBytes, 0x14, 0x12345678u);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x4E800020u,               // return before proven method
                    0x38630001u, 0x48000008u, // proven method -> common tail
                    0x38630002u, 0x48000008u, // sibling tail thunk
                    0x38630003u, 0x48000004u, // sibling tail thunk
                    0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);
        var pointerTables = StaticFunctionPointerTableDiscovery.Discover(dol, image);

        var result = StoredNullHeaderVTableDiscovery.Discover(
            pointerTables,
            dol,
            image,
            new HashSet<uint> { text + 0x04 },
            new HashSet<uint>(),
            new HashSet<uint> { data });

        Assert.Equal(new[] { text + 0x0Cu, text + 0x14u }, result.Targets);
        Assert.Equal(1, result.TableSegmentCount);
    }

    [Fact]
    public void StoredStridedDescriptorsPromoteReturnDelimitedSiblingEvenWhenPreviouslyCovered()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;
        var dataBytes = new byte[0x40];

        // Three {0,0,function} interface descriptors. Code stores data+0x0C as
        // an object field and dispatch later reads +8, selecting text+0x10.
        WriteU32(dataBytes, 0x00, 0);
        WriteU32(dataBytes, 0x04, 0);
        WriteU32(dataBytes, 0x08, text + 0x08);
        WriteU32(dataBytes, 0x0C, 0);
        WriteU32(dataBytes, 0x10, 0);
        WriteU32(dataBytes, 0x14, text + 0x10);
        WriteU32(dataBytes, 0x18, 0);
        WriteU32(dataBytes, 0x1C, 0);
        WriteU32(dataBytes, 0x20, text + 0x18);
        WriteU32(dataBytes, 0x24, 0x12345678u);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x38600000u, 0x4E800020u,
                    0x38630001u, 0x4E800020u,
                    0x38630002u, 0x4E800020u,
                    0x38630003u, 0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);

        var result = StoredStridedCallbackDescriptorDiscovery.Discover(
            dol,
            image,
            new HashSet<uint> { text + 0x08 },
            new HashSet<uint> { data + 0x0C });

        Assert.Equal(new[] { text + 0x10, text + 0x18 }, result.Targets);
        Assert.Equal(1, result.TableSegmentCount);
        Assert.Equal(1, result.CandidateTableCount);
    }

    [Fact]
    public void StridedDescriptorShapeWithoutStoredObjectPointerIsNotAnOracle()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;
        var dataBytes = new byte[0x30];
        for (var i = 0; i < 3; ++i)
        {
            WriteU32(dataBytes, i * 12 + 0, 0);
            WriteU32(dataBytes, i * 12 + 4, 0);
            WriteU32(dataBytes, i * 12 + 8, text + (uint)(i * 8));
        }

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x38600001u, 0x4E800020u,
                    0x38600002u, 0x4E800020u,
                    0x38600003u, 0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);

        var result = StoredStridedCallbackDescriptorDiscovery.Discover(
            dol,
            image,
            new HashSet<uint>(),
            new HashSet<uint>());

        Assert.Empty(result.Targets);
        Assert.Equal(0, result.TableSegmentCount);
        Assert.Equal(1, result.CandidateTableCount);
    }

    [Fact]
    public void StoredStridedDescriptorRequiresDenseBoundaryEvidence()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;
        var dataBytes = new byte[0x30];
        WriteU32(dataBytes, 0x00, 0); WriteU32(dataBytes, 0x04, 0); WriteU32(dataBytes, 0x08, text + 0x04);
        WriteU32(dataBytes, 0x0C, 0); WriteU32(dataBytes, 0x10, 0); WriteU32(dataBytes, 0x14, text + 0x08);
        WriteU32(dataBytes, 0x18, 0); WriteU32(dataBytes, 0x1C, 0); WriteU32(dataBytes, 0x20, text + 0x0C);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    // Only text+4 follows a terminal transfer. The other two
                    // targets are interior instructions, so 1/3 evidence fails.
                    0x4E800020u,
                    0x38630001u,
                    0x38630002u,
                    0x38630003u,
                    0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);

        var result = StoredStridedCallbackDescriptorDiscovery.Discover(
            dol,
            image,
            new HashSet<uint>(),
            new HashSet<uint> { data });

        Assert.Empty(result.Targets);
        Assert.Equal(0, result.TableSegmentCount);
    }

    [Fact]
    public void CallArgumentConstantDiscoveryKeepsPhiSelectedTableBases()
    {
        var function = new IrFunction(
            "caller",
            "entry",
            new[]
            {
                new IrBasicBlock(
                    "entry",
                    new IrInstruction[]
                    {
                        new IrAssign("r26_1", IrValue.Imm(unchecked((int)0x80670000u))),
                        new IrBinary("r5_a", IrValue.Register("r26_1"), IrValue.Imm(-20216), "add"),
                        new IrBinary("r5_b", IrValue.Register("r26_1"), IrValue.Imm(-20272), "add"),
                        new IrPhi("r5_3", new Dictionary<string, string>
                        {
                            ["left"] = "r5_a",
                            ["right"] = "r5_b",
                        }),
                        new IrCall(string.Empty, "0x8056E47C", new[] { IrValue.Register("r5_3") }),
                    })
            });

        var values = CallArgumentConstantDiscovery.Discover(function);

        Assert.Contains(0x8066B108u, values);
        Assert.Contains(0x8066B0D0u, values);
    }

    [Fact]
    public void PassedSparseCallbackTableRequiresExactPassedBaseAndTerminalTargets()
    {
        const uint text = 0x80001000u;
        const uint data = 0x80002000u;
        var dataBytes = new byte[0x40];

        // Four independent callbacks with two null slots. Every target begins
        // immediately after the previous callback's blr (the first follows the
        // sentinel blr at text+0). This mirrors a sparse dispatcher array, not a
        // dense switch table.
        WriteU32(dataBytes, 0x10, text + 0x04);
        WriteU32(dataBytes, 0x14, text + 0x0C);
        WriteU32(dataBytes, 0x18, 0);
        WriteU32(dataBytes, 0x1C, text + 0x14);
        WriteU32(dataBytes, 0x20, text + 0x1C);
        WriteU32(dataBytes, 0x24, 0);
        WriteU32(dataBytes, 0x28, 0x12345678u);

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x4E800020u,
                    0x38600001u, 0x4E800020u,
                    0x38600002u, 0x4E800020u,
                    0x38600003u, 0x4E800020u,
                    0x38600004u, 0x4E800020u),
                SyntheticDolFactory.Data(0, data, dataBytes)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);

        var notPassed = PassedSparseCallbackTableDiscovery.Discover(
            dol, image, new HashSet<uint>());
        Assert.Empty(notPassed.Targets);

        var passed = PassedSparseCallbackTableDiscovery.Discover(
            dol, image, new HashSet<uint> { data + 0x10 });
        Assert.Equal(
            new[] { text + 0x04, text + 0x0C, text + 0x14, text + 0x1C },
            passed.Targets);
        Assert.Equal(1, passed.TableSegmentCount);
        Assert.Equal(1, passed.CandidateTableCount);
    }

    [Fact]
    public void StoredConstantPointerDiscoveryRequiresANearbyStraightLineStore()
    {
        static PpcRegisterOperand R(int number) => new($"r{number}", number);
        static PpcImmediateOperand I(int value) => new(value);

        var instructions = new[]
        {
            PpcInstruction.Synthetic(0x80001000, 0, "lis", new PpcOperand[] { R(5), I(unchecked((short)0x806A)) }),
            PpcInstruction.Synthetic(0x80001004, 0, "li", new PpcOperand[] { R(31), I(0) }),
            PpcInstruction.Synthetic(0x80001008, 0, "addi", new PpcOperand[] { R(5), R(5), I(-31924) }),
            PpcInstruction.Synthetic(0x8000100C, 0, "stw", new PpcOperand[] { R(5), new PpcDisplacementOperand(312, "r3", 3) }),

            // A constant does not survive a control-flow boundary merely because
            // the decoded instruction list happens to continue afterwards.
            PpcInstruction.Synthetic(0x80001010, 0, "lis", new PpcOperand[] { R(6), I(unchecked((short)0x806A)) }),
            PpcInstruction.Synthetic(
                0x80001014,
                0,
                "b",
                new PpcOperand[] { new PpcBranchTargetOperand(0x80001020) },
                branchTargets: new[] { 0x80001020u }),
            PpcInstruction.Synthetic(0x80001020, 0, "addi", new PpcOperand[] { R(7), R(6), I(-30020) }),
            PpcInstruction.Synthetic(0x80001024, 0, "stw", new PpcOperand[] { R(7), new PpcDisplacementOperand(108, "r3", 3) }),

            // A perfectly materialized constant written only to the stack is not
            // constructor/vptr evidence.
            PpcInstruction.Synthetic(0x80001028, 0, "lis", new PpcOperand[] { R(8), I(unchecked((short)0x806A)) }),
            PpcInstruction.Synthetic(0x8000102C, 0, "addi", new PpcOperand[] { R(8), R(8), I(-30020) }),
            PpcInstruction.Synthetic(0x80001030, 0, "stw", new PpcOperand[] { R(8), new PpcDisplacementOperand(12, "r1", 1) })
        };

        var stored = StoredConstantPointerDiscovery.Discover(instructions);
        Assert.Equal([0x8069834Cu], stored);
        Assert.DoesNotContain(0x80698ABCu, stored);
    }

    [Fact]
    public void StoredConstantPointerDiscoveryAllowsConstructorBookkeepingBeforeVptrStore()
    {
        static PpcRegisterOperand R(int number) => new($"r{number}", number);
        static PpcImmediateOperand I(int value) => new(value);

        // Mirrors the live CodeWarrior shape around 802FF52C..802FF550:
        // lis/addi materializes the vptr, then saves registers/initializes another
        // field before finally storing that same constant into the object.
        var instructions = new[]
        {
            PpcInstruction.Synthetic(0x80001000, 0, "lis", new PpcOperand[] { R(6), I(unchecked((short)0x806A)) }),
            PpcInstruction.Synthetic(0x80001004, 0, "addi", new PpcOperand[] { R(6), R(6), I(0x19F0) }),
            PpcInstruction.Synthetic(0x80001008, 0, "stw", new PpcOperand[] { R(31), new PpcDisplacementOperand(12, "r1", 1) }),
            PpcInstruction.Synthetic(0x8000100C, 0, "mr", new PpcOperand[] { R(31), R(5) }),
            PpcInstruction.Synthetic(0x80001010, 0, "stw", new PpcOperand[] { R(30), new PpcDisplacementOperand(8, "r1", 1) }),
            PpcInstruction.Synthetic(0x80001014, 0, "mr", new PpcOperand[] { R(30), R(3) }),
            PpcInstruction.Synthetic(0x80001018, 0, "stw", new PpcOperand[] { R(0), new PpcDisplacementOperand(4, "r3", 3) }),
            PpcInstruction.Synthetic(0x8000101C, 0, "stw", new PpcOperand[] { R(6), new PpcDisplacementOperand(0, "r3", 3) }),
        };

        Assert.Equal([0x806A19F0u], StoredConstantPointerDiscovery.Discover(instructions));
    }

    [Fact]
    public void StoredPointerTracksAddicR0ThroughInitializationStores()
    {
        var words = new List<uint> { 0x3C008000u, 0x34001004u }; // lis r0; addic. r0,r0,0x1004
        for (var i = 0; i < 8; i++) words.Add(0x93DD0400u + (uint)i * 4); // stw r30,+offset(r29)
        words.Add(0x901D0E20u); // stw r0,3616(r29)
        var instructions = words.Select((w, i) => PpcDecoder.Decode(0x80003000u + (uint)i * 4, w)).ToArray();
        Assert.Contains(0x80001004u, StoredConstantPointerDiscovery.Discover(instructions));
    }

    [Fact]
    public void StoredCodeBoundaryRequiresPointerTerminalPredecessorAndNoCoveredFlow()
    {
        const uint text = 0x80001000u;
        var dol = SyntheticDolFactory.Create(text, sections: [SyntheticDolFactory.Text(0, text,
            0x4E800020u, 0x4E800020u, 0x38600001u, 0x4E800020u, 0x4E800020u)]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);
        var pointers = new HashSet<uint> { text + 4, text + 12, text + 16 };
        var ends = new HashSet<uint> { text + 4, text + 12, text + 16 };
        Assert.Equal(new[] { text + 4 }, StoredFunctionBoundaryDiscovery.Discover(
            dol, image, pointers, ends, new HashSet<uint> { text + 16 }));
        Assert.Empty(StoredFunctionBoundaryDiscovery.Discover(dol, image,
            new HashSet<uint>(), ends, new HashSet<uint>()));
        Assert.Empty(StoredFunctionBoundaryDiscovery.Discover(dol, image,
            pointers, new HashSet<uint>(), new HashSet<uint>()));
    }

    [Fact]
    public void StoredIdentityCallbackCanFollowAnotherUncoveredReturnAtADecodedEnd()
    {
        const uint text = 0x80001000u;
        var dol = SyntheticDolFactory.Create(text, sections: [SyntheticDolFactory.Text(0, text,
            0x4E800020u, 0x4E800020u, 0x4E800020u)]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);
        Assert.Equal(new[] { text + 8 }, StoredFunctionBoundaryDiscovery.Discover(dol, image,
            new HashSet<uint> { text + 8 }, new HashSet<uint> { text + 4 }, new HashSet<uint>()));
        Assert.Empty(StoredFunctionBoundaryDiscovery.Discover(dol, image,
            new HashSet<uint> { text + 8 }, new HashSet<uint> { text + 4 }, new HashSet<uint> { text + 4 }));
    }

    [Fact]
    public void SdaCallbackRegistrationRequiresSetterConsumerAndAllOrZeroMaskedCodePointer()
    {
        static PpcRegisterOperand R(int number) => new($"r{number}", number);
        static PpcImmediateOperand I(int value) => new(value);

        const uint text = 0x80001000u;
        const uint callback = text + 0x04;
        const uint setterAddress = 0x80566848u;
        const int slot = -7136;

        var dol = SyntheticDolFactory.Create(
            text,
            sections:
            [
                SyntheticDolFactory.Text(
                    0,
                    text,
                    0x4E800020u, // terminal predecessor
                    0x38600001u,
                    0x4E800020u)
            ]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);

        var setter = SdaCallbackRegistrationDiscovery.AnalyzeFunction(
            setterAddress,
            new[]
            {
                PpcInstruction.Synthetic(
                    setterAddress,
                    0,
                    "stw",
                    new PpcOperand[] { R(3), new PpcDisplacementOperand(slot, "r13", 13) }),
                PpcInstruction.Synthetic(setterAddress + 4, 0x4E800020u, "blr", Array.Empty<PpcOperand>()),
            });

        var consumer = SdaCallbackRegistrationDiscovery.AnalyzeFunction(
            0x80568A2Cu,
            new[]
            {
                PpcInstruction.Synthetic(
                    0x80568A6Cu,
                    0,
                    "lwz",
                    new PpcOperand[] { R(12), new PpcDisplacementOperand(slot, "r13", 13) }),
                PpcInstruction.Synthetic(0x80568A70u, 0, "cmpwi", new PpcOperand[] { R(12), I(0) }),
                PpcInstruction.Synthetic(
                    0x80568A74u,
                    0,
                    "beq",
                    new PpcOperand[] { new PpcBranchTargetOperand(0x80568A94u) },
                    branchTargets: new[] { 0x80568A94u },
                    isConditional: true),
                PpcInstruction.Synthetic(0x80568A78u, 0, "mr", new PpcOperand[] { R(3), R(31) }),
                PpcInstruction.Synthetic(0x80568A7Cu, 0, "mtctr", new PpcOperand[] { R(12) }),
                PpcInstruction.Synthetic(0x80568A80u, 0, "bctrl", Array.Empty<PpcOperand>(), isCall: true),
            });

        var caller = SdaCallbackRegistrationDiscovery.AnalyzeFunction(
            0x80423D6Cu,
            new[]
            {
                PpcInstruction.Synthetic(0x8042423Cu, 0, "lis", new PpcOperand[] { R(3), I(unchecked((short)0x8000)) }),
                PpcInstruction.Synthetic(0x80424240u, 0, "addi", new PpcOperand[] { R(3), R(3), I(0x1004) }),
                PpcInstruction.Synthetic(0x80424244u, 0, "subfic", new PpcOperand[] { R(0), R(0), I(0) }),
                PpcInstruction.Synthetic(0x80424248u, 0, "subfe", new PpcOperand[] { R(0), R(0), R(0) }),
                PpcInstruction.Synthetic(0x8042424Cu, 0, "and", new PpcOperand[] { R(3), R(3), R(0) }),
                PpcInstruction.Synthetic(
                    0x80424250u,
                    0,
                    "bl",
                    new PpcOperand[] { new PpcBranchTargetOperand(setterAddress) },
                    branchTargets: new[] { setterAddress },
                    isCall: true),
            });

        var result = SdaCallbackRegistrationDiscovery.Resolve(dol, image, new[] { setter, consumer, caller });

        Assert.Equal([callback], result.Targets);
        Assert.Equal(1, result.SetterCount);
        Assert.Equal(1, result.ConsumedSlotCount);
        Assert.Equal(1, result.RegistrationCount);
    }

    [Fact]
    public void SdaCallbackRegistrationRejectsArbitraryPointerMask()
    {
        static PpcRegisterOperand R(int number) => new($"r{number}", number);
        static PpcImmediateOperand I(int value) => new(value);

        const uint text = 0x80001000u;
        const uint setterAddress = 0x80566848u;
        const int slot = -7136;
        var dol = SyntheticDolFactory.Create(
            text,
            sections: [SyntheticDolFactory.Text(0, text, 0x4E800020u, 0x38600001u, 0x4E800020u)]);
        var image = new ProgramImageBuilder().Build(dol, ramSize: 0x10000);

        var setter = SdaCallbackRegistrationDiscovery.AnalyzeFunction(
            setterAddress,
            new[]
            {
                PpcInstruction.Synthetic(setterAddress, 0, "stw", new PpcOperand[] { R(3), new PpcDisplacementOperand(slot, "r13", 13) }),
                PpcInstruction.Synthetic(setterAddress + 4, 0x4E800020u, "blr", Array.Empty<PpcOperand>()),
            });
        var consumer = SdaCallbackRegistrationDiscovery.AnalyzeFunction(
            0x80568A2Cu,
            new[]
            {
                PpcInstruction.Synthetic(0x80568A6Cu, 0, "lwz", new PpcOperand[] { R(12), new PpcDisplacementOperand(slot, "r13", 13) }),
                PpcInstruction.Synthetic(0x80568A7Cu, 0, "mtctr", new PpcOperand[] { R(12) }),
                PpcInstruction.Synthetic(0x80568A80u, 0, "bctrl", Array.Empty<PpcOperand>(), isCall: true),
            });
        var arbitraryMaskCaller = SdaCallbackRegistrationDiscovery.AnalyzeFunction(
            0x80423D6Cu,
            new[]
            {
                PpcInstruction.Synthetic(0x8042423Cu, 0, "lis", new PpcOperand[] { R(3), I(unchecked((short)0x8000)) }),
                PpcInstruction.Synthetic(0x80424240u, 0, "addi", new PpcOperand[] { R(3), R(3), I(0x1004) }),
                PpcInstruction.Synthetic(0x8042424Cu, 0, "and", new PpcOperand[] { R(3), R(3), R(0) }),
                PpcInstruction.Synthetic(
                    0x80424250u,
                    0,
                    "bl",
                    new PpcOperand[] { new PpcBranchTargetOperand(setterAddress) },
                    branchTargets: new[] { setterAddress },
                    isCall: true),
            });

        Assert.Empty(SdaCallbackRegistrationDiscovery.Resolve(
            dol,
            image,
            new[] { setter, consumer, arbitraryMaskCaller }).Targets);
    }

    [Fact]
    public void SdaCallbackSetterRequiresExactBlrTail()
    {
        static PpcRegisterOperand R(int number) => new($"r{number}", number);
        const int slot = -7136;
        const uint setterAddress = 0x80566848u;

        var bctrTail = SdaCallbackRegistrationDiscovery.AnalyzeFunction(
            setterAddress,
            new[]
            {
                PpcInstruction.Synthetic(setterAddress, 0, "stw", new PpcOperand[] { R(3), new PpcDisplacementOperand(slot, "r13", 13) }),
                PpcInstruction.Synthetic(setterAddress + 4, 0x4E800420u, "bctr", Array.Empty<PpcOperand>()),
            });

        Assert.Null(bctrTail.Setter);
    }

    [Fact]
    public void SdaCallbackConsumerRejectsMtctrClobberBeforeBctrl()
    {
        static PpcRegisterOperand R(int number) => new($"r{number}", number);
        const int slot = -7136;

        var consumer = SdaCallbackRegistrationDiscovery.AnalyzeFunction(
            0x80568A2Cu,
            new[]
            {
                PpcInstruction.Synthetic(0x80568A6Cu, 0, "lwz", new PpcOperand[] { R(12), new PpcDisplacementOperand(slot, "r13", 13) }),
                PpcInstruction.Synthetic(0x80568A70u, 0, "mtctr", new PpcOperand[] { R(12) }),
                PpcInstruction.Synthetic(0x80568A74u, 0, "mtctr", new PpcOperand[] { R(11) }),
                PpcInstruction.Synthetic(0x80568A78u, 0, "bctrl", Array.Empty<PpcOperand>()),
            });

        Assert.Empty(consumer.ConsumedSdaOffsets);
    }

    [Fact]
    public void SdaCallbackConsumerRejectsDirectCallBeforeBctrl()
    {
        static PpcRegisterOperand R(int number) => new($"r{number}", number);
        const int slot = -7136;

        var consumer = SdaCallbackRegistrationDiscovery.AnalyzeFunction(
            0x80568A2Cu,
            new[]
            {
                PpcInstruction.Synthetic(0x80568A6Cu, 0, "lwz", new PpcOperand[] { R(12), new PpcDisplacementOperand(slot, "r13", 13) }),
                PpcInstruction.Synthetic(0x80568A70u, 0, "mtctr", new PpcOperand[] { R(12) }),
                PpcInstruction.Synthetic(
                    0x80568A74u,
                    0,
                    "bl",
                    new PpcOperand[] { new PpcBranchTargetOperand(0x80510000u) },
                    branchTargets: new[] { 0x80510000u },
                    isCall: true),
                PpcInstruction.Synthetic(0x80568A78u, 0, "bctrl", Array.Empty<PpcOperand>()),
            });

        Assert.Empty(consumer.ConsumedSdaOffsets);
    }

    private static void WriteU32(byte[] bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(offset, 4), value);
}
