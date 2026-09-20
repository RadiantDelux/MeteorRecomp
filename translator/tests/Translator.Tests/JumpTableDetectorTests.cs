using System.Collections.Generic;
using Translator.Core.Disassembly;
using Xunit;

namespace Translator.Tests;

public class JumpTableDetectorTests
{
    private static IReadOnlyDictionary<uint, int> BuildIndex(IReadOnlyList<PpcInstruction> ordered)
        => ordered.Select((ins, idx) => new { ins.Address, idx }).ToDictionary(x => x.Address, x => x.idx);

    [Fact]
    public void RecognizesLwzxBackedSwitchTable()
    {
        const uint baseAddr = 0x80000000;
        const uint tableAddr = 0x80000100;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "slwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x18, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var indexByAddress = BuildIndex(ordered);

        var image = TranslatorCppTestHarness.CreateImage(
            (tableAddr + 0x00, baseAddr + 0x40),
            (tableAddr + 0x04, baseAddr + 0x50),
            (tableAddr + 0x08, baseAddr + 0x60));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            indexByAddress,
            baseAddr + 0x18,
            baseAddr,
            baseAddr + 0x100,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(new uint[] { baseAddr + 0x40, baseAddr + 0x50, baseAddr + 0x60 }, targets);
    }

    [Fact]
    public void UsesFallbackUpperBoundWhenCompareUsesLogicalCounter()
    {
        const uint baseAddr = 0x80001000;
        const uint tableAddr = 0x80001200;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r18", 18), new PpcImmediateOperand(1) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "slwi", new PpcOperand[] { new PpcRegisterOperand("r30", 30), new PpcRegisterOperand("r18", 18), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x1200) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x18, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var indexByAddress = BuildIndex(ordered);

        var image = TranslatorCppTestHarness.CreateImage(
            (tableAddr + 0x00, baseAddr + 0x30),
            (tableAddr + 0x04, baseAddr + 0x34));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            indexByAddress,
            baseAddr + 0x18,
            baseAddr,
            baseAddr + 0x100,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(new uint[] { baseAddr + 0x30, baseAddr + 0x34 }, targets);
    }

    [Fact]
    public void ScaledIndexPrefersSelectorCompareOverStaleTemporaryCompare()
    {
        const uint baseAddr = 0x80006000;
        const uint tableAddr = 0x80006200;
        var ordered = new List<PpcInstruction>
        {
            // An unrelated older lifetime of r0.  The old detector found this
            // first because r0 is the lwzx byte-offset register and truncated
            // the table to two entries.
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmpwi", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcImmediateOperand(1) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r4", 4), new PpcImmediateOperand(4) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "rlwinm", new PpcOperand[]
            {
                new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r4", 4),
                new PpcImmediateOperand(2), new PpcImmediateOperand(0), new PpcImmediateOperand(29)
            }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x6200) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x18, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x1C, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var image = TranslatorCppTestHarness.CreateImage(
            (tableAddr + 0x00, baseAddr + 0x40),
            (tableAddr + 0x04, baseAddr + 0x44),
            (tableAddr + 0x08, baseAddr + 0x48),
            (tableAddr + 0x0C, baseAddr + 0x4C),
            (tableAddr + 0x10, baseAddr + 0x50));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x1C,
            baseAddr,
            baseAddr + 0x100,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(
            new uint[] { baseAddr + 0x40, baseAddr + 0x44, baseAddr + 0x48, baseAddr + 0x4C, baseAddr + 0x50 },
            targets);
    }

    [Fact]
    public void RecognizesLargeRetailStateMachineTable()
    {
        // RDSPAF has this exact shape in func_801BC07C: cmplwi selector,1159;
        // selector*4; lwzx from an absolute table; mtctr/bctr.  The historical
        // 512-entry safety cap caused the table to fall back to an indirect
        // runtime jump even though all entries are local continuations.
        const uint baseAddr = 0x801BC07C;
        const uint tableAddr = 0x80379458;
        const int upperBound = 1159;
        const uint caseA = baseAddr + 0x4C;
        const uint caseB = baseAddr + 0x80;

        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x2C, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r5", 5), new PpcImmediateOperand(upperBound) }),
            PpcInstruction.Synthetic(baseAddr + 0x34, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r6", 6), new PpcImmediateOperand(unchecked((short)0x8038)) }),
            PpcInstruction.Synthetic(baseAddr + 0x38, 0, "rlwinm", new PpcOperand[]
            {
                new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r5", 5),
                new PpcImmediateOperand(2), new PpcImmediateOperand(0), new PpcImmediateOperand(29)
            }),
            PpcInstruction.Synthetic(baseAddr + 0x3C, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r6", 6), new PpcRegisterOperand("r6", 6), new PpcImmediateOperand(unchecked((short)-27560)) }),
            PpcInstruction.Synthetic(baseAddr + 0x40, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r6", 6), new PpcRegisterOperand("r6", 6), new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x44, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r6", 6) }),
            PpcInstruction.Synthetic(baseAddr + 0x48, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var words = new (uint Address, uint Value)[upperBound + 1];
        for (var i = 0; i <= upperBound; i++)
        {
            words[i] = (tableAddr + (uint)(i * 4), (i & 1) == 0 ? caseA : caseB);
        }

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x48,
            baseAddr,
            baseAddr + 0x100,
            TranslatorCppTestHarness.CreateImage(words),
            out var targets);

        Assert.True(recognized);
        Assert.Equal(new uint[] { caseA, caseB }, targets);
    }

    [Fact]
    public void RecognizesPcRelativeTablePointerWithRelativeEntries()
    {
        const uint baseAddr = 0x80010000;
        const uint linkAddress = baseAddr + 0x04;
        const uint picBase = 0x80011000;
        const uint tableAddr = 0x80012000;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "bl", new PpcOperand[] { new PpcBranchTargetOperand(linkAddress) }, [linkAddress], isCall: true),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "mflr", new PpcOperand[] { new PpcRegisterOperand("r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lwz", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcDisplacementOperand(-20, "r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "add", new PpcOperand[] { new PpcRegisterOperand("r30", 30), new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcImmediateOperand(1) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "slwi", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r9", 9), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(baseAddr + 0x18, 0, "lwz", new PpcOperand[] { new PpcRegisterOperand("r10", 10), new PpcDisplacementOperand(-0x20, "r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x1C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r10", 10), new PpcRegisterOperand("r9", 9) }),
            PpcInstruction.Synthetic(baseAddr + 0x20, 0, "add", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r10", 10) }),
            PpcInstruction.Synthetic(baseAddr + 0x24, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r9", 9) }),
            PpcInstruction.Synthetic(baseAddr + 0x28, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var image = TranslatorCppTestHarness.CreateImage(
            (linkAddress - 20, unchecked(picBase - linkAddress)),
            (picBase - 0x20, tableAddr),
            (tableAddr + 0x00, unchecked((baseAddr + 0x40) - tableAddr)),
            (tableAddr + 0x04, unchecked((baseAddr + 0x50) - tableAddr)));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x28,
            baseAddr,
            baseAddr + 0x100,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(new uint[] { baseAddr + 0x40, baseAddr + 0x50 }, targets);
    }

    [Fact]
    public void RecognizesPcRelativeTablePointerWhenTocSetupIsFarBack()
    {
        const uint baseAddr = 0x80018000;
        const uint linkAddress = baseAddr + 0x04;
        const uint picBase = 0x8001A000;
        const uint tableAddr = 0x8001B000;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "bl", new PpcOperand[] { new PpcBranchTargetOperand(linkAddress) }, [linkAddress], isCall: true),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "mflr", new PpcOperand[] { new PpcRegisterOperand("r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lwz", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcDisplacementOperand(-20, "r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "add", new PpcOperand[] { new PpcRegisterOperand("r30", 30), new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r30", 30) }),
        };

        for (var i = 0; i < 76; i++)
        {
            ordered.Add(PpcInstruction.Synthetic(baseAddr + 0x10 + (uint)(i * 4), 0, "nop", System.Array.Empty<PpcOperand>()));
        }

        var switchBase = baseAddr + 0x10 + (76u * 4);
        ordered.AddRange(new[]
        {
            PpcInstruction.Synthetic(switchBase + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcImmediateOperand(1) }),
            PpcInstruction.Synthetic(switchBase + 0x04, 0, "slwi", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r9", 9), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(switchBase + 0x08, 0, "lwz", new PpcOperand[] { new PpcRegisterOperand("r10", 10), new PpcDisplacementOperand(-0x20, "r30", 30) }),
            PpcInstruction.Synthetic(switchBase + 0x0C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r10", 10), new PpcRegisterOperand("r9", 9) }),
            PpcInstruction.Synthetic(switchBase + 0x10, 0, "add", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r10", 10) }),
            PpcInstruction.Synthetic(switchBase + 0x14, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r9", 9) }),
            PpcInstruction.Synthetic(switchBase + 0x18, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        });

        var image = TranslatorCppTestHarness.CreateImage(
            (linkAddress - 20, unchecked(picBase - linkAddress)),
            (picBase - 0x20, tableAddr),
            (tableAddr + 0x00, unchecked((baseAddr + 0x180) - tableAddr)),
            (tableAddr + 0x04, unchecked((baseAddr + 0x190) - tableAddr)));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            switchBase + 0x18,
            baseAddr,
            baseAddr + 0x200,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(new uint[] { baseAddr + 0x180, baseAddr + 0x190 }, targets);
    }

    [Fact]
    public void RecognizesAbsoluteTableBaseRegisterWhenLisIsFarBack()
    {
        // Rabbids Go Home's K3D HLSL loader initializes r23 once near the
        // function prologue, then uses it over 300 decoded instructions later
        // as the base of a 14-entry switch table at 0x80551C50.
        const uint functionStart = 0x80323E2C;
        const uint lisAddress = 0x80323E64;
        const uint tableAddress = 0x80551C50;
        const uint bctrAddress = 0x8032438C;
        const uint functionEnd = 0x803244B8;

        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(
                lisAddress,
                0,
                "lis",
                new PpcOperand[] { new PpcRegisterOperand("r23", 23), new PpcImmediateOperand(unchecked((short)0x8055)) }),
        };

        // Preserve the real lifetime distance: the old 192-instruction search
        // could resolve the earlier sibling table in this function, but not this
        // one because r23's lis is roughly 328 decoded instructions behind lwzx.
        for (var i = 0; i < 320; i++)
        {
            ordered.Add(PpcInstruction.Synthetic(lisAddress + 4u + (uint)(i * 4), 0, "nop", System.Array.Empty<PpcOperand>()));
        }

        ordered.AddRange(new[]
        {
            PpcInstruction.Synthetic(0x80324368u, 0, "lbz", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcDisplacementOperand(5, "r21", 21) }),
            PpcInstruction.Synthetic(0x8032436Cu, 0, "lwz", new PpcOperand[] { new PpcRegisterOperand("r4", 4), new PpcDisplacementOperand(12, "r21", 21) }),
            PpcInstruction.Synthetic(0x80324370u, 0, "subi", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(3) }),
            PpcInstruction.Synthetic(0x80324374u, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcImmediateOperand(13) }),
            PpcInstruction.Synthetic(0x8032437Cu, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcRegisterOperand("r23", 23), new PpcImmediateOperand(0x1C50) }),
            PpcInstruction.Synthetic(0x80324380u, 0, "rlwinm", new PpcOperand[]
            {
                new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r0", 0),
                new PpcImmediateOperand(2), new PpcImmediateOperand(0), new PpcImmediateOperand(29)
            }),
            PpcInstruction.Synthetic(0x80324384u, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcRegisterOperand("r3", 3), new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(0x80324388u, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(bctrAddress, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        });

        var image = TranslatorCppTestHarness.CreateImage(
            (tableAddress + 0x00, 0x80324390u),
            (tableAddress + 0x04, 0x80324390u),
            (tableAddress + 0x08, 0x80324398u),
            (tableAddress + 0x0C, 0x803243A0u),
            (tableAddress + 0x10, 0x803243A8u),
            (tableAddress + 0x14, 0x803243A8u),
            (tableAddress + 0x18, 0x803243B0u),
            (tableAddress + 0x1C, 0x803243B8u),
            (tableAddress + 0x20, 0x803243C0u),
            (tableAddress + 0x24, 0x803243C0u),
            (tableAddress + 0x28, 0x803243C0u),
            (tableAddress + 0x2C, 0x803243C0u),
            (tableAddress + 0x30, 0x803243A0u),
            (tableAddress + 0x34, 0x803243A8u));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            bctrAddress,
            functionStart,
            functionEnd,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(
            new uint[] { 0x80324390u, 0x80324398u, 0x803243A0u, 0x803243A8u, 0x803243B0u, 0x803243B8u, 0x803243C0u },
            targets);
    }

    [Fact]
    public void RejectsWhenEntryPointOrMtctrChainIsMissing()
    {
        const uint baseAddr = 0x80002000;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var image = TranslatorCppTestHarness.CreateImage((0x80002100u, baseAddr + 0x20));

        Assert.False(JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x10,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));

        Assert.False(JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x04,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));
    }

    [Fact]
    public void RejectsWhenCtrLoadChainIsInvalid()
    {
        const uint baseAddr = 0x80003000;
        const uint tableAddr = 0x80003100;
        var image = TranslatorCppTestHarness.CreateImage((tableAddr + 0x00, baseAddr + 0x20));

        var wrongMtctrOperand = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(0) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "mtctr", System.Array.Empty<PpcOperand>()),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        Assert.False(JumpTableDetector.TryRecognize(
            wrongMtctrOperand,
            BuildIndex(wrongMtctrOperand),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));

        var wrongLoadDest = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(0) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "lbzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        Assert.False(JumpTableDetector.TryRecognize(
            wrongLoadDest,
            BuildIndex(wrongLoadDest),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));
    }

    [Fact]
    public void RejectsWhenTableBaseOrUpperBoundCannotBeResolved()
    {
        const uint baseAddr = 0x80004000;
        const uint tableAddr = 0x80004100;
        var image = TranslatorCppTestHarness.CreateImage((tableAddr + 0x00, baseAddr + 0x20));

        var unresolvedBase = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(0) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r11", 11), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        Assert.False(JumpTableDetector.TryRecognize(
            unresolvedBase,
            BuildIndex(unresolvedBase),
            baseAddr + 0x10,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));

        var oversizeBound = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(5000) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        Assert.False(JumpTableDetector.TryRecognize(
            oversizeBound,
            BuildIndex(oversizeBound),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));
    }

    [Fact]
    public void RejectsUnreadableAndOutOfWindowTargets()
    {
        const uint baseAddr = 0x80005000;
        const uint tableAddr = 0x80005100;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(1) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var missingEntryImage = TranslatorCppTestHarness.CreateImage((tableAddr + 0x00, baseAddr + 0x20));
        Assert.False(JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            missingEntryImage,
            out _));

        var outOfWindowImage = TranslatorCppTestHarness.CreateImage(
            (tableAddr + 0x00, 0x90000000u),
            (tableAddr + 0x04, baseAddr + 0x20));
        Assert.False(JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            outOfWindowImage,
            out _));
    }
}
