using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Tests;

public sealed class PairedFlowCodeGenTests
{
    [Fact]
    public void MixedScalarAndPairedPredecessorsPreservePayloadAndTagTheEdge()
    {
        var function = new IrFunction(
            "mixed_ps_merge",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBranch("beq", "paired", "scalar")
                }),
                new IrBasicBlock("paired", new IrInstruction[]
                {
                    new IrCall("f5", "PPC_PsMul", new[]
                    {
                        IrValue.Register("f1"), IrValue.Register("f2")
                    }),
                    new IrJump("merge")
                }),
                new IrBasicBlock("scalar", new IrInstruction[]
                {
                    new IrAssign("f5", IrValue.Register("f3")),
                    new IrJump("merge")
                }),
                new IrBasicBlock("merge", new IrInstruction[]
                {
                    new IrCall("f6", "PPC_PsNeg", new[] { IrValue.Register("f5") }),
                    new IrReturn(null)
                })
            });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f5"] = ValueRepresentation.Float64,
            ["f6"] = ValueRepresentation.Float64,
            ["cr0"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("mixed_ps_merge", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006000, new SsaTransformer().Convert(function), signature, types);

        Assert.DoesNotContain("f5.d = PPC_PsToScalarInline(f5.d);", code, StringComparison.Ordinal);
        Assert.Contains("mkw_paired_state |= 0x00000020u;", code, StringComparison.Ordinal);
        Assert.Contains("mkw_paired_state &= ~0x00000020u;", code, StringComparison.Ordinal);
        Assert.Contains("? f5.d : PPC_PsFromScalarInline(f5.d)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void FloatingCrCompareExtractsPs0FromPairedOperands()
    {
        var function = new IrFunction("paired_fcmp", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("f2", "PPC_PsMul", new[] { IrValue.Register("f3"), IrValue.Register("f4") }),
                new IrCall("f5", "PPC_PsAdd", new[] { IrValue.Register("f6"), IrValue.Register("f7") }),
                new IrSetCrField(0, IrValue.Register("f2"), IrValue.Register("f5"), false),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(Enumerable.Range(0, 8)
            .ToDictionary(index => $"f{index}", _ => (ValueRepresentation)ValueRepresentation.Float64));
        var signature = new FunctionAbiClassification("paired_fcmp", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006004, new SsaTransformer().Convert(function), signature, types);

        Assert.Contains(
            "SetCRFloatResident(cr, 0, PPC_PsToScalarInline(f2.d), PPC_PsToScalarInline(f5.d));",
            code, StringComparison.Ordinal);
    }

    [Fact]
    public void FloatingCrCompareLeavesScalarOperandsUnchanged()
    {
        var function = new IrFunction("scalar_fcmp", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(3, IrValue.Register("f1"), IrValue.Register("f2"), false),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("scalar_fcmp", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006004, new SsaTransformer().Convert(function), signature, types);

        Assert.Contains("SetCRFloatResident(cr, 3, f1.d, f2.d);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void ExternalEntryStartsScalarButPairedBackedgeRetainsBothLanes()
    {
        var function = new IrFunction(
            "entry_backedge_ps_state",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall(string.Empty, "PPC_Fcmp", new[] { IrValue.Imm(0), IrValue.Register("f31"), IrValue.Register("f1") }),
                    new IrBranch("beq", "loop", "exit")
                }),
                new IrBasicBlock("loop", new IrInstruction[]
                {
                    new IrCall("f31", "PPC_PsMul", new[] { IrValue.Register("f2"), IrValue.Register("f3") }),
                    new IrJump("entry")
                }),
                new IrBasicBlock("exit", new IrInstruction[] { new IrReturn(null) })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64,
            ["cr0"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("entry_backedge_ps_state", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80006008, ssa, signature, types);

        Assert.Contains("uint32_t mkw_paired_state = 0u;", code, StringComparison.Ordinal);
        Assert.Contains("mkw_paired_state |= 0x80000000u;", code, StringComparison.Ordinal);
        Assert.Contains("? PPC_PsToScalarInline(f31.d) : f31.d", code, StringComparison.Ordinal);
        Assert.DoesNotContain("f31.d = PPC_PsToScalarInline(f31.d);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void DispatchLoopDoesNotScalarizeMatrixPairsBetweenCommands()
    {
        var function = new IrFunction("matrix_commands", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[] { new IrJump("dispatch") }),
            new IrBasicBlock("dispatch", new IrInstruction[]
            {
                new IrBranch("beq", "load", "use")
            }),
            new IrBasicBlock("load", new IrInstruction[]
            {
                new IrCall("f31", "PPC_PsMerge00", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                new IrJump("dispatch")
            }),
            new IrBasicBlock("use", new IrInstruction[]
            {
                new IrCall("f4", "PPC_PsMuls0", new[] { IrValue.Register("f31"), IrValue.Register("f3") }),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(Enumerable.Range(0, 32)
            .ToDictionary(i => $"f{i}", _ => (ValueRepresentation)ValueRepresentation.Float64));
        var code = new CxxLinearCodeGenerator().Emit(0x80006010,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("matrix_commands", ValueRepresentation.Void), types);
        Assert.Contains("mkw_paired_state |= 0x80000000u;", code, StringComparison.Ordinal);
        Assert.Contains("? f31.d : PPC_PsFromScalarInline(f31.d)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("f31.d = PPC_PsToScalarInline(f31.d);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void MixedFprCopyPropagatesTagAndScalarConsumerDoesNotRoundDoubleArm()
    {
        var function = new IrFunction("mixed_copy", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[] { new IrBranch("beq", "paired", "scalar") }),
            new IrBasicBlock("paired", new IrInstruction[]
            {
                new IrCall("f5", "PPC_PsMerge00", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                new IrJump("merge")
            }),
            new IrBasicBlock("scalar", new IrInstruction[]
            {
                new IrAssign("f5", IrValue.Register("f3")), new IrJump("merge")
            }),
            new IrBasicBlock("merge", new IrInstruction[]
            {
                new IrAssign("f7", IrValue.Register("f5")),
                new IrBinary("f8", IrValue.Register("f7"), IrValue.Register("f0"), "add"),
                new IrCall("f6", "PPC_PsNeg", new[] { IrValue.Register("f7") }),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(Enumerable.Range(0, 9)
            .ToDictionary(i => $"f{i}", _ => (ValueRepresentation)ValueRepresentation.Float64));
        var code = new CxxLinearCodeGenerator().Emit(0x80006014,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("mixed_copy", ValueRepresentation.Void), types);
        Assert.Contains("mkw_paired_state >> 5", code, StringComparison.Ordinal);
        Assert.Contains("? PPC_PsToScalarInline(f7.d) : f7.d", code, StringComparison.Ordinal);
        Assert.Contains("? f7.d : PPC_PsFromScalarInline(f7.d)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("f5.d = PPC_PsFromScalarInline(f5.d);", code, StringComparison.Ordinal);
    }
}
