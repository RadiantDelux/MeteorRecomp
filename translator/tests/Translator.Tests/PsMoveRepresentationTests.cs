using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Tests;

public sealed class PsMoveRepresentationTests
{
    [Theory]
    [InlineData(4)] // lfs: the host payload is the widened scalar, not packed floats.
    [InlineData(8)] // lfd: a move must also retain all double precision.
    public void ScalarLoadMoveKeepsScalarStoresAndConvertsOnlyAtPairedConsumer(int loadSize)
    {
        var code = Emit(new IrBasicBlock("entry", new IrInstruction[]
        {
            new IrLoad("f5", new IrAddress("r3", 0), loadSize),
            Move("f7", "f5"),
            new IrStore(new IrAddress("r3", 16), IrValue.Register("f7"), 4),
            new IrStore(new IrAddress("r3", 24), IrValue.Register("f7"), 8),
            new IrCall("f8", "PPC_PsNeg", new[] { IrValue.Register("f7") }),
            new IrReturn(null)
        }));

        Assert.Contains("PpcSetPairedFprInline(f7, f5.d);", code, StringComparison.Ordinal);
        Assert.Contains("Float32((r3 + 16), f7.d);", code, StringComparison.Ordinal);
        Assert.Contains("Float64((r3 + 24), f7.d);", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsNegInline(PPC_PsFromScalarInline(f7.d))", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsFromScalarInline(f5.d)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsToScalarInline(f7.d)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void AlreadyPairedMoveKeepsBothLanesAcrossBlockBoundary()
    {
        var code = Emit(
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Pair("f5"), Move("f7", "f5"), new IrJump("use")
            }),
            new IrBasicBlock("use", Consumers("f7")));

        Assert.Contains("PpcSetPairedFprInline(f7, f5.d);", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsNegInline(f7.d)", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsToScalarInline(f7.d)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsFromScalarInline(f7.d)", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void MixedMovePreservesPayloadAndTagAcrossSuccessor(bool selfMove)
    {
        var destination = selfMove ? "f5" : "f7";
        var code = Emit(
            new IrBasicBlock("entry", new IrInstruction[] { new IrBranch("beq", "paired", "scalar") }),
            new IrBasicBlock("paired", new IrInstruction[] { Pair("f5"), new IrJump("merge") }),
            new IrBasicBlock("scalar", new IrInstruction[]
            {
                new IrLoad("f5", new IrAddress("r3", 0), 8), new IrJump("merge")
            }),
            new IrBasicBlock("merge", new IrInstruction[] { Move(destination, "f5"), new IrJump("use") }),
            new IrBasicBlock("use", Consumers(destination)));

        Assert.Contains($"PpcSetPairedFprInline({destination}, f5.d);", code, StringComparison.Ordinal);
        Assert.Contains("mkw_paired_state |= 0x00000020u;", code, StringComparison.Ordinal);
        Assert.Contains("mkw_paired_state &= ~0x00000020u;", code, StringComparison.Ordinal);
        if (!selfMove)
            Assert.Contains("mkw_paired_state = (mkw_paired_state & ~0x00000080u) | (((mkw_paired_state >> 5) & 1u) << 7);", code, StringComparison.Ordinal);
        Assert.Contains($"? {destination}.d : PPC_PsFromScalarInline({destination}.d)", code, StringComparison.Ordinal);
        Assert.Contains($"? PPC_PsToScalarInline({destination}.d) : {destination}.d", code, StringComparison.Ordinal);
        Assert.DoesNotContain("f5.d = PPC_PsFromScalarInline(f5.d);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("f5.d = PPC_PsToScalarInline(f5.d);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void EntryBackedgeMoveKeepsExternalScalarAndLoopPair()
    {
        var code = Emit(
            new IrBasicBlock("entry", new IrInstruction[]
            {
                Move("f7", "f5"),
                new IrStore(new IrAddress("r3", 16), IrValue.Register("f7"), 4),
                new IrBranch("beq", "loop", "exit")
            }),
            new IrBasicBlock("loop", new IrInstruction[] { Pair("f5"), new IrJump("entry") }),
            new IrBasicBlock("exit", new IrInstruction[]
            {
                new IrCall("f8", "PPC_PsNeg", new[] { IrValue.Register("f7") }), new IrReturn(null)
            }));

        Assert.Contains("uint32_t mkw_paired_state = 0u;", code, StringComparison.Ordinal);
        Assert.Contains("mkw_paired_state >> 5", code, StringComparison.Ordinal);
        Assert.Contains("? PPC_PsToScalarInline(f7.d) : f7.d", code, StringComparison.Ordinal);
        Assert.Contains("? f7.d : PPC_PsFromScalarInline(f7.d)", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void SelfMoveKeepsKnownSourceRepresentation(bool paired)
    {
        var code = Emit(new IrBasicBlock("entry", new IrInstruction[]
        {
            paired ? Pair("f5") : new IrLoad("f5", new IrAddress("r3", 0), 4),
            Move("f5", "f5"),
            new IrCall("f8", "PPC_PsNeg", new[] { IrValue.Register("f5") }),
            new IrReturn(null)
        }));

        Assert.Contains("PpcSetPairedFprInline(f5, f5.d);", code, StringComparison.Ordinal);
        Assert.Contains(paired ? "PPC_PsNegInline(f5.d)" : "PPC_PsNegInline(PPC_PsFromScalarInline(f5.d))", code, StringComparison.Ordinal);
    }

    private static IrCall Move(string destination, string source) =>
        new(destination, "PPC_PsMr", new[] { IrValue.Register(source) });

    private static IrCall Pair(string destination) =>
        new(destination, "PPC_PsMerge00", new[] { IrValue.Register("f1"), IrValue.Register("f2") });

    private static IrInstruction[] Consumers(string source) => new IrInstruction[]
    {
        new IrStore(new IrAddress("r3", 16), IrValue.Register(source), 4),
        new IrCall("f8", "PPC_PsNeg", new[] { IrValue.Register(source) }),
        new IrReturn(null)
    };

    private static string Emit(params IrBasicBlock[] blocks)
    {
        var types = Enumerable.Range(0, 32).ToDictionary(i => $"f{i}",
            _ => (ValueRepresentation)ValueRepresentation.Float64, StringComparer.OrdinalIgnoreCase);
        types["r3"] = ValueRepresentation.UInt32;
        types["cr0"] = ValueRepresentation.UInt32;
        return new CxxLinearCodeGenerator().Emit(0x80006240,
            new SsaTransformer().Convert(new IrFunction("ps_move_representation", "entry", blocks)),
            new FunctionAbiClassification("ps_move_representation", ValueRepresentation.Void),
            new RepresentationEnvironment(types));
    }
}
